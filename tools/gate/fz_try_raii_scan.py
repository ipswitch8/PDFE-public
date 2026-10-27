#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AST-based sweep for RAII objects that cross a MuPDF setjmp/longjmp boundary.

WHY THIS IS NOT A GREP
----------------------
MuPDF's ``fz_try``/``fz_always``/``fz_catch`` are implemented with
``setjmp``/``longjmp``.  A ``longjmp`` out of a guarded region does NOT run
destructors for locals in the frames it skips, so a local with a non-trivial
destructor declared *inside* such a region leaks whatever it owns whenever
MuPDF throws.  A C++ ``throw`` raised inside the region is the mirror image: it
unwinds through MuPDF's C frames, which are not unwind-aware.

Two separate questions have to be answered, and text answers neither:

1. *Which code is inside a guarded region?*  ``fz_try`` occurs 121 times in
   this tree, but 49 of those are inside comments and 9 more are prose in
   headers.  Worse, the LARGER real population contains no ``fz_try`` token at
   all: every ``MuPdfContext::execute`` / ``executeReturn`` lambda body runs
   inside an ``fz_try`` that lives in ``MuPdfContext``.
2. *Does this local have a non-trivial destructor?*  ``std::string s;`` and
   ``fz_rect r;`` are indistinguishable to a regex, and the answer depends on
   the whole type graph -- a struct with no destructor of its own is still
   non-trivially destructible if any member is.

So this tool asks the compiler both questions.

HOW EACH QUESTION IS ANSWERED
-----------------------------
* **Regions** come from clang's preprocessing record -- real macro expansions
  of ``fz_try``/``fz_always``/``fz_catch``, so a token inside a comment can
  never appear -- and from the AST for ``execute``/``executeReturn`` lambda
  bodies, resolved through the callee's semantic parent so an unrelated method
  called ``execute`` is not picked up.
* **Destructor triviality** is answered by the C++ front end itself, via
  ``__is_trivially_destructible``, evaluated in the translation unit the
  variable actually lives in.  libclang cannot answer this: it does not expose
  the members of implicit template instantiations, so asking it directly
  reports ``std::string`` as trivially destructible.  That was measured, not
  assumed -- see ``self_test``, which pins the answer for ``std::string``,
  ``std::vector<int>``, ``std::unique_ptr<int>`` and a struct whose only
  non-trivial part is a member.

Types that cannot be named at namespace scope (lambda closure types, local and
anonymous structs) cannot be put in a ``static_assert``, so those fall back to
an AST computation of [class.dtor]/8 -- which is sound for them, because they
are never implicit template instantiations.  Anything still undecided is
reported as ``undecidable_type`` rather than being folded into "fine".

WHAT IT DELIBERATELY DOES NOT CLAIM
-----------------------------------
A call to a function that itself constructs RAII objects and can throw is not
reported: that is interprocedural and undecidable from one translation unit.

**A raw owning pointer released by hand is not reported either** -- an
``fz_buffer*`` freed with ``fz_drop_buffer`` at the end of a guarded block is
neither a non-trivially-destructible VarDecl nor a ``throw``, so it is invisible
here even though a longjmp leaks it just the same. Two such sites were found by
review during phase-68, not by this tool. Deciding them needs an
ownership/must-release analysis, which is a different tool; saying so is better
than letting the hazard count imply a completeness it does not have.

Implicit throws are likewise absent: ``v.assign(...)`` can raise
``std::bad_alloc`` and only an explicit ``throw`` expression is matched.

This tool is a lower bound -- but a lower bound on a population derived from
the compiler, and it never reports a comment.
"""

from __future__ import annotations

import argparse
import io
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import tempfile

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

try:
    import clang.cindex as ci
except ImportError:  # pragma: no cover - reported, never silently skipped
    sys.stderr.write(
        "fz_try_raii_scan: the 'libclang' python package is not installed.\n"
        "                 install it with:  python -m pip install libclang\n")
    raise

CursorKind = ci.CursorKind
TypeKind = ci.TypeKind

#: Macros that open and close a MuPDF guarded region.
OPEN_MACRO = "fz_try"
CLOSE_MACROS = ("fz_always", "fz_catch")

#: The lambda-taking members of MuPdfContext.  Their bodies run inside an
#: fz_try that lives in MuPdfContext, so a local declared in such a body is on
#: exactly the same longjmp path as one declared inside a literal fz_try.
EXECUTE_METHODS = ("execute", "executeReturn")
EXECUTE_CLASS = "MuPdfContext"

HAZARD_NONTRIVIAL = "nontrivial_dtor_local"
HAZARD_THROW = "cxx_throw"
HAZARD_UNKNOWN = "undecidable_type"

TRIVIAL = "trivial"
NONTRIVIAL = "nontrivial"
UNKNOWN = "unknown"

DEFAULT_CLANG_CL = (
    r"C:\Program Files\Microsoft Visual Studio\2022\Community"
    r"\VC\Tools\Llvm\x64\bin\clang-cl.exe")


# --------------------------------------------------------------------------
# cheap, certain classifications (no compiler round-trip needed)
# --------------------------------------------------------------------------

_TRIVIAL_KINDS = {
    TypeKind.VOID, TypeKind.BOOL, TypeKind.CHAR_U, TypeKind.UCHAR,
    TypeKind.CHAR16, TypeKind.CHAR32, TypeKind.USHORT, TypeKind.UINT,
    TypeKind.ULONG, TypeKind.ULONGLONG, TypeKind.UINT128, TypeKind.CHAR_S,
    TypeKind.SCHAR, TypeKind.WCHAR, TypeKind.SHORT, TypeKind.INT,
    TypeKind.LONG, TypeKind.LONGLONG, TypeKind.INT128, TypeKind.FLOAT,
    TypeKind.DOUBLE, TypeKind.LONGDOUBLE, TypeKind.NULLPTR,
    TypeKind.POINTER, TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE,
    TypeKind.MEMBERPOINTER, TypeKind.ENUM, TypeKind.FUNCTIONPROTO,
    TypeKind.FUNCTIONNOPROTO,
}

_ARRAY_KINDS = {
    TypeKind.CONSTANTARRAY, TypeKind.VARIABLEARRAY,
    TypeKind.DEPENDENTSIZEDARRAY, TypeKind.INCOMPLETEARRAY,
}

#: Spelling fragments that mean "this type has no name usable in a
#: static_assert at namespace scope".
_UNNAMEABLE = ("(lambda at", "(unnamed", "(anonymous")


def quick_class(t):
    """TRIVIAL for builtins/pointers/enums (and arrays of them), else None.

    Only certainties live here: a pointer never has a destructor, whatever it
    points at.  Everything else is deferred to the compiler.
    """
    t = t.get_canonical()
    if t.kind in _ARRAY_KINDS:
        return quick_class(t.element_type)
    if t.kind in _TRIVIAL_KINDS:
        return TRIVIAL
    return None


def ast_destructor_class(t, _depth=0, _seen=None):
    """[class.dtor]/8 computed from the AST -- the fallback for unnameable types.

    Only sound where the record is a real, fully-visible definition; libclang
    does not expose the members of implicit template instantiations, which is
    why this is NOT used as the general oracle.
    """
    if _seen is None:
        _seen = set()
    if _depth > 24:
        return UNKNOWN

    t = t.get_canonical()
    if t.kind in _ARRAY_KINDS:
        return ast_destructor_class(t.element_type, _depth + 1, _seen)
    if t.kind in _TRIVIAL_KINDS:
        return TRIVIAL
    if t.kind in (TypeKind.UNEXPOSED, TypeKind.DEPENDENT):
        return UNKNOWN
    if t.kind != TypeKind.RECORD:
        return TRIVIAL

    decl = t.get_declaration()
    if decl is None:
        return UNKNOWN
    usr = decl.get_usr() or (t.spelling + "@" + str(decl.hash))
    if usr in _seen:
        return TRIVIAL
    _seen = _seen | {usr}

    definition = decl.get_definition() or decl
    if not definition.is_definition():
        return UNKNOWN

    worst = TRIVIAL
    saw_member = False
    for child in definition.get_children():
        if child.kind == CursorKind.DESTRUCTOR:
            saw_member = True
            if child.is_virtual_method() or not child.is_default_method():
                return NONTRIVIAL
        elif child.kind in (CursorKind.CXX_BASE_SPECIFIER, CursorKind.FIELD_DECL):
            saw_member = True
            r = ast_destructor_class(child.type, _depth + 1, _seen)
            if r == NONTRIVIAL:
                return NONTRIVIAL
            if r == UNKNOWN:
                worst = UNKNOWN
    if not saw_member:
        # A record with no visible members at all is either genuinely empty or
        # an instantiation libclang will not expand. The caller knows which,
        # because it only reaches here for types it could not name.
        return worst
    return worst


# --------------------------------------------------------------------------
# compile_commands handling
# --------------------------------------------------------------------------

_MSVC_INCLUDE_CACHE = []


def msvc_include_args(verbose=False):
    """``-imsvc`` flags for the MSVC + Windows SDK headers.

    The Ninja build runs inside a Visual Studio developer environment, so its
    compile_commands entries do NOT carry the standard-library include paths --
    they arrive through %INCLUDE%.  libclang inherits no such environment, and
    clang-cl's MSVC autodetection does not find them here, so without this
    every translation unit fails with "'string' file not found" and the scan
    reports zero hazards while looking exactly like a clean tree.  That failure
    mode is the one this project has been bitten by before, so the paths are
    taken from the same vcvars64.bat the build itself uses, and
    ``--allow-parse-errors`` is the only way to proceed past a parse failure.
    """
    if _MSVC_INCLUDE_CACHE:
        return _MSVC_INCLUDE_CACHE[0]
    bat = os.environ.get("PDFE_VCVARS") or (
        r"C:\Program Files\Microsoft Visual Studio\2022\Community"
        r"\VC\Auxiliary\Build\vcvars64.bat")
    args = []
    if os.path.exists(bat):
        try:
            proc = subprocess.run(
                ["cmd.exe", "/c", "call", bat, ">nul", "&&", "set", "INCLUDE"],
                capture_output=True, text=True, timeout=180)
            for line in (proc.stdout or "").splitlines():
                if line.upper().startswith("INCLUDE="):
                    for d in line.split("=", 1)[1].split(";"):
                        d = d.strip()
                        if d and os.path.isdir(d):
                            args.append("-imsvc" + d)
        except Exception as exc:  # pragma: no cover - reported below
            if verbose:
                sys.stderr.write("msvc_include_args: %r\n" % (exc,))
    if verbose and not args:
        sys.stderr.write(
            "msvc_include_args: could not read %%INCLUDE%% from %s\n" % bat)
    _MSVC_INCLUDE_CACHE.append(args)
    return args


_DROP_EXACT = {"-c", "--", "/nologo", "-nologo"}
_DROP_PREFIX = ("/Fo", "-Fo", "/Fd", "-Fd", "/Fp", "-Fp", "@")


def _split_command(command):
    """Split a compile_commands 'command' string, honouring quotes."""
    out, cur, quote = [], "", None
    for ch in command:
        if quote:
            if ch == quote:
                quote = None
            else:
                cur += ch
        elif ch in "\"'":
            quote = ch
        elif ch.isspace():
            if cur:
                out.append(cur)
                cur = ""
        else:
            cur += ch
    if cur:
        out.append(cur)
    return out


def _looks_like_path(a):
    return ("\\" in a or "/" in a or ":" in a) and not a.startswith(("-", "/"))


def parse_args_for(entry, with_msvc=True):
    """Turn one compile_commands entry into libclang / clang-cl arguments."""
    argv = list(entry["arguments"]) if "arguments" in entry \
        else _split_command(entry["command"])
    if argv:
        argv = argv[1:]  # drop the compiler
    src = _norm(entry["file"])
    keep = []
    for a in argv:
        if a in _DROP_EXACT or a.startswith(_DROP_PREFIX):
            continue
        if _looks_like_path(a) and _norm(a) == src:
            continue
        keep.append(a)
    # clang-cl flag spellings (/EHsc, -TP, -imsvc...) are only understood in cl
    # driver mode; the compile database was produced by clang-cl.
    return ["--driver-mode=cl"] + keep + (msvc_include_args() if with_msvc else [])


def load_compile_commands(path, repo_root, subdirs=("src",)):
    with io.open(path, encoding="utf-8") as fh:
        db = json.load(fh)
    root = _norm(repo_root)
    wanted = tuple(os.path.join(root, s) + os.sep for s in subdirs)
    seen = {}
    for e in db:
        f = _norm(e["file"])
        if not f.startswith(wanted):
            continue
        if "_autogen" in f or f.endswith("mocs_compilation.cpp"):
            continue
        if not f.endswith((".cpp", ".cc", ".cxx", ".c")):
            # CMake puts the Windows resource script (.rc) in the compile
            # database too. It is not a C++ translation unit and libclang
            # cannot parse it; excluding it here is why it is not counted as
            # a parse error, which would otherwise mask a real one.
            continue
        if f in seen:
            continue
        seen[f] = e
    return [seen[k] for k in sorted(seen)]


# --------------------------------------------------------------------------
# region discovery
# --------------------------------------------------------------------------

class Region(object):
    __slots__ = ("kind", "file", "start", "end", "start_line", "end_line", "detail")

    def __init__(self, kind, file, start, end, start_line, end_line, detail=""):
        self.kind = kind
        self.file = file
        self.start = start
        self.end = end
        self.start_line = start_line
        self.end_line = end_line
        self.detail = detail

    def as_dict(self):
        return {"kind": self.kind, "file": self.file,
                "start_line": self.start_line, "end_line": self.end_line,
                "detail": self.detail}


def _norm(path):
    return os.path.normcase(os.path.abspath(path)) if path else ""


def fz_try_regions(tu, in_tree):
    """Regions from the preprocessing record: real fz_try macro expansions.

    A region runs from the end of the fz_try expansion to the start of the next
    fz_always/fz_catch expansion in the same file.  Tokens inside comments are
    not macro expansions, so they never appear here -- which is the point.
    """
    opens, closes = {}, {}
    for c in tu.cursor.walk_preorder():
        if c.kind != CursorKind.MACRO_INSTANTIATION:
            continue
        name = c.spelling
        if name != OPEN_MACRO and name not in CLOSE_MACROS:
            continue
        ext = c.extent
        f = _norm(ext.start.file.name) if ext.start.file else ""
        if not in_tree(f):
            continue
        if name == OPEN_MACRO:
            opens.setdefault(f, []).append((ext.end.offset, ext.start.line))
        else:
            closes.setdefault(f, []).append((ext.start.offset, ext.start.line))

    regions = []
    for f, lst in opens.items():
        ends = sorted(closes.get(f, []))
        for off, line in sorted(lst):
            nxt = next(((c, l) for c, l in ends if c > off), None)
            if nxt is None:
                # An fz_try with no fz_always/fz_catch after it is itself a
                # defect; report it rather than dropping the region.
                regions.append(Region("fz_try", f, off, off + 1, line, line,
                                      "unterminated: no fz_always/fz_catch follows"))
            else:
                regions.append(Region("fz_try", f, off, nxt[0], line, nxt[1]))
    return regions


def execute_lambda_regions(tu, in_tree, strict_class=True):
    """Regions from MuPdfContext::execute / executeReturn lambda bodies."""
    regions = []
    for call in tu.cursor.walk_preorder():
        if call.kind != CursorKind.CALL_EXPR or call.spelling not in EXECUTE_METHODS:
            continue
        ref = call.referenced
        owner = ""
        if ref is not None and ref.semantic_parent is not None:
            owner = ref.semantic_parent.spelling
        if strict_class and owner != EXECUTE_CLASS:
            continue
        for node in call.walk_preorder():
            if node.kind != CursorKind.LAMBDA_EXPR:
                continue
            body = None
            for ch in node.get_children():
                if ch.kind == CursorKind.COMPOUND_STMT:
                    body = ch
            if body is None:
                continue
            ext = body.extent
            f = _norm(ext.start.file.name) if ext.start.file else ""
            if not in_tree(f):
                continue
            regions.append(Region(
                "execute_lambda", f, ext.start.offset, ext.end.offset,
                ext.start.line, ext.end.line,
                "%s(...) lambda body%s" % (
                    call.spelling,
                    "" if owner else " [callee class unresolved]")))
    return regions


# --------------------------------------------------------------------------
# the triviality oracle: the C++ front end itself
# --------------------------------------------------------------------------

_PROBE_MARK = "PDFE_DTOR_PROBE"
_DIAG_RE = re.compile(r"[(\[](\d+)[,:]\d+[)\]]?\s*:\s*(?:fatal\s+)?error\s*:\s*(.*)",
                      re.I)


def classify_types_with_compiler(source_file, args, spellings, clang_cl=None,
                                 keep_probe=None):
    """Ask the compiler whether each named type is trivially destructible.

    The probe is the ORIGINAL translation unit with static_asserts appended, so
    every type is evaluated in the exact scope it was found in -- including
    types from anonymous namespaces, which no standalone probe could name.

    Returns {spelling: TRIVIAL|NONTRIVIAL|UNKNOWN}. A type whose assert fails
    is non-trivially destructible; a type whose line produces any OTHER
    diagnostic is UNKNOWN, never silently trivial.
    """
    if not spellings:
        return {}
    clang_cl = clang_cl or os.environ.get("PDFE_CLANG_CL") or DEFAULT_CLANG_CL
    if not os.path.exists(clang_cl):
        return {s: UNKNOWN for s in spellings}

    try:
        with io.open(source_file, encoding="utf-8", errors="replace") as fh:
            body = fh.read()
    except OSError:
        return {s: UNKNOWN for s in spellings}

    ordered = sorted(spellings)
    lines = body.split("\n")
    base = len(lines)
    if lines and lines[-1] == "":
        base -= 1
    extra = ["", "// ---- appended by tools/gate/fz_try_raii_scan.py ----"]
    line_of = {}
    for i, spell in enumerate(ordered):
        extra.append('static_assert(__is_trivially_destructible(%s), "%s_%d");'
                     % (spell, _PROBE_MARK, i))
        line_of[base + len(extra)] = i

    tmpdir = tempfile.mkdtemp(prefix="fz_probe_")
    try:
        probe = os.path.join(tmpdir, "probe_" + os.path.basename(source_file))
        with io.open(probe, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(body.rstrip("\n") + "\n" + "\n".join(extra) + "\n")
        cmd = ([clang_cl]
               + [a for a in args if a != "--driver-mode=cl"]
               + ["-I" + os.path.dirname(os.path.abspath(source_file)),
                  "-fsyntax-only", "-ferror-limit=0", probe])
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        out = (proc.stdout or "") + (proc.stderr or "")
    except Exception as exc:  # pragma: no cover - surfaced as UNKNOWN
        shutil.rmtree(tmpdir, ignore_errors=True)
        return {s: UNKNOWN for s in spellings}

    if keep_probe:
        shutil.copyfile(probe, keep_probe)
    shutil.rmtree(tmpdir, ignore_errors=True)

    result = {s: TRIVIAL for s in ordered}
    probe_name = os.path.basename(source_file)
    for line in out.splitlines():
        if probe_name not in line and "probe_" not in line:
            continue
        m = _DIAG_RE.search(line)
        if not m:
            continue
        idx = line_of.get(int(m.group(1)))
        if idx is None:
            continue
        msg = m.group(2)
        if "static assertion failed" in msg or "static_assert failed" in msg:
            result[ordered[idx]] = NONTRIVIAL
        else:
            result[ordered[idx]] = UNKNOWN
    return result


# --------------------------------------------------------------------------
# hazard discovery
# --------------------------------------------------------------------------

def scan_tu(entry, repo_root, clang_cl=None):
    root = _norm(repo_root)

    def in_tree(path):
        return bool(path) and path.startswith(root + os.sep)

    args = parse_args_for(entry)
    index = ci.Index.create()
    try:
        tu = index.parse(entry["file"], args=args,
                         options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
    except ci.TranslationUnitLoadError as exc:
        return {"file": entry["file"], "error": str(exc), "regions": [],
                "hazards": []}

    fatals = [d.spelling for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]

    regions = fz_try_regions(tu, in_tree) + execute_lambda_regions(tu, in_tree)
    by_file = {}
    for r in regions:
        by_file.setdefault(r.file, []).append(r)

    # pass 1: collect candidates and the set of type spellings to ask about
    candidates, need_compiler, ast_fallback = [], set(), {}
    seen_keys = set()
    for node in tu.cursor.walk_preorder():
        loc = node.location
        if loc.file is None:
            continue
        f = _norm(loc.file.name)
        cands = by_file.get(f)
        if not cands:
            continue
        off = loc.offset
        inside = [r for r in cands if r.start <= off < r.end]
        if not inside:
            continue
        region = min(inside, key=lambda r: r.end - r.start)

        if node.kind == CursorKind.CXX_THROW_EXPR:
            key = (f, loc.line, loc.column, HAZARD_THROW, "")
            if key in seen_keys:
                continue
            seen_keys.add(key)
            candidates.append({"cursor_kind": "throw", "file": f, "line": loc.line,
                               "column": loc.column, "name": "", "spelling": "",
                               "region": region})
            continue
        if node.kind != CursorKind.VAR_DECL:
            continue
        if node.storage_class in (ci.StorageClass.STATIC, ci.StorageClass.EXTERN):
            continue
        if quick_class(node.type) == TRIVIAL:
            continue
        spell = node.type.get_canonical().spelling
        key = (f, loc.line, loc.column, "var", node.spelling)
        if key in seen_keys:
            continue
        seen_keys.add(key)
        if any(u in spell for u in _UNNAMEABLE):
            ast_fallback[spell] = ast_destructor_class(node.type)
        else:
            need_compiler.add(spell)
        candidates.append({"cursor_kind": "var", "file": f, "line": loc.line,
                           "column": loc.column, "name": node.spelling,
                           "spelling": spell, "region": region})

    verdicts = dict(ast_fallback)
    if need_compiler and not fatals:
        verdicts.update(classify_types_with_compiler(
            entry["file"], args, need_compiler, clang_cl))
    elif need_compiler:
        verdicts.update({s: UNKNOWN for s in need_compiler})

    hazards = []
    for c in candidates:
        if c["cursor_kind"] == "throw":
            kind, detail = HAZARD_THROW, \
                "throw expression crosses the setjmp/longjmp boundary"
        else:
            v = verdicts.get(c["spelling"], UNKNOWN)
            if v == TRIVIAL:
                continue
            kind = HAZARD_NONTRIVIAL if v == NONTRIVIAL else HAZARD_UNKNOWN
            detail = c["spelling"]
        hazards.append({
            "file": os.path.relpath(c["file"], root).replace("\\", "/"),
            "line": c["line"], "column": c["column"], "hazard": kind,
            "name": c["name"], "type": detail,
            "region": c["region"].kind, "region_line": c["region"].start_line,
            "region_detail": c["region"].detail,
        })
    hazards.sort(key=lambda h: (h["file"], h["line"], h["column"]))

    return {
        "file": os.path.relpath(_norm(entry["file"]), root).replace("\\", "/"),
        "error": "; ".join(fatals) if fatals else "",
        "regions": [r.as_dict() for r in regions],
        "hazards": hazards,
    }


def _worker(payload):
    entry, repo_root, clang_cl = payload
    try:
        return scan_tu(entry, repo_root, clang_cl)
    except Exception as exc:  # pragma: no cover - surfaced, never swallowed
        return {"file": entry["file"], "error": "scan crashed: %r" % (exc,),
                "regions": [], "hazards": []}


def run_scan(db_path, repo_root, jobs=None, subdirs=("src",), files=None,
             clang_cl=None, progress=False):
    entries = load_compile_commands(db_path, repo_root, subdirs)
    if files:
        wanted = {_norm(f) for f in files}
        entries = [e for e in entries if _norm(e["file"]) in wanted]
    payloads = [(e, repo_root, clang_cl) for e in entries]
    jobs = jobs or max(1, (os.cpu_count() or 2) - 1)
    results = []
    if jobs == 1:
        for i, p in enumerate(payloads):
            results.append(_worker(p))
            if progress:
                print("  [%d/%d] %s" % (i + 1, len(payloads), results[-1]["file"]))
    else:
        with multiprocessing.Pool(jobs) as pool:
            for i, r in enumerate(pool.imap_unordered(_worker, payloads, chunksize=1)):
                results.append(r)
                if progress:
                    print("  [%d/%d] %s" % (i + 1, len(payloads), r["file"]))
                    sys.stdout.flush()

    hazards, regions, errors = [], 0, []
    region_kinds, hazard_kinds = {}, {}
    for r in results:
        hazards.extend(r["hazards"])
        regions += len(r["regions"])
        for reg in r["regions"]:
            region_kinds[reg["kind"]] = region_kinds.get(reg["kind"], 0) + 1
        if r["error"]:
            errors.append({"file": r["file"], "error": r["error"][:400]})
    for h in hazards:
        # Broken out because "how many hazards a token-based sweep could even
        # reach" is the number this whole tool exists to correct, and a total
        # alone does not answer it.
        key = h["region"] + "/" + h["hazard"]
        hazard_kinds[key] = hazard_kinds.get(key, 0) + 1
    hazards.sort(key=lambda h: (h["file"], h["line"], h["column"]))
    return {"translation_units": len(results), "regions": regions,
            "region_kinds": region_kinds, "hazard_kinds": hazard_kinds,
            "hazard_count": len(hazards), "hazards": hazards, "errors": errors}


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

_FIXTURE = r'''
#include <string>
#include <vector>
#include <memory>

struct Trivial { int a; double b; };
struct WithDtor { ~WithDtor(); };
struct DefaultedDtor { ~DefaultedDtor() = default; int x; };
struct HasNonTrivialField { std::string s; };
struct HasTrivialField { Trivial t; int a[4]; };
struct VirtualDtor { virtual ~VirtualDtor(); };
struct DerivedFromNonTrivial : WithDtor { int y; };
struct DerivedFromTrivial : Trivial { int y; };
enum E { E0 };

void trivial_probe() {
    Trivial a; DefaultedDtor b; HasTrivialField c; int d; double e;
    E f; int* g; int h[8]; DerivedFromTrivial i;
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
}

void nontrivial_probe() {
    std::string a; std::vector<int> b; WithDtor c; HasNonTrivialField d;
    VirtualDtor* pe = nullptr; DerivedFromNonTrivial f;
    std::unique_ptr<int> g; std::vector<std::string> h;
    (void)a; (void)b; (void)c; (void)d; (void)pe; (void)f; (void)g; (void)h;
}
'''

_TRIVIAL_EXPECTED = {"a", "b", "c", "d", "e", "f", "g", "h", "i"}
_NONTRIVIAL_EXPECTED = {"a", "b", "c", "d", "f", "g", "h"}  # 'pe' is a pointer

_REGION_FIXTURE = (
    "struct Owning { Owning(); ~Owning(); };\n"
    "#define fz_try(c) if (1)\n"
    "#define fz_always(c) else if (0)\n"
    "#define fz_catch(c) if (0)\n"
    "struct Ctx {};\n"
    "struct MuPdfContext {\n"
    "  template <class F> void execute(F f) { f((Ctx*)0); }\n"
    "  template <class R, class F> R executeReturn(F f) { return f((Ctx*)0); }\n"
    "};\n"
    "struct NotTheContext {\n"
    "  template <class F> void execute(F f) { f((Ctx*)0); }\n"
    "};\n"
    "// fz_try(ctx) { Owning commented_out; }\n"
    "void f(MuPdfContext* mc, NotTheContext* other) {\n"
    "  Owning outside_ok;\n"
    "  Ctx* ctx = nullptr;\n"
    "  fz_try(ctx) { Owning inside_try; (void)inside_try; }\n"
    "  fz_always(ctx) { }\n"
    "  fz_catch(ctx) { }\n"
    "  mc->execute([](Ctx* c){ Owning inside_lambda; (void)inside_lambda; });\n"
    "  int n = mc->executeReturn<int>([](Ctx* c){ Owning in_ret;"
    " (void)in_ret; return 1; });\n"
    "  other->execute([](Ctx* c){ Owning unrelated_execute;"
    " (void)unrelated_execute; });\n"
    "  (void)n; (void)outside_ok;\n"
    "}\n")


def stdlib_args_from_db(db_path, repo_root):
    """Compile arguments good enough to resolve <string>/<vector>.

    The type-classification half of the self-test is only worth anything if it
    runs against the REAL standard library this project builds with; a
    hand-rolled stand-in would only prove the tool agrees with itself.  So the
    arguments come from the project's own compile database, and if it is
    missing those checks FAIL rather than silently not running.
    """
    if not os.path.exists(db_path):
        return None
    entries = load_compile_commands(db_path, repo_root)
    if not entries:
        return None
    args = parse_args_for(entries[0])
    return [a for a in args
            if not a.startswith(("-fsanitize", "/Zi", "-Zi", "/RTC", "-O", "/O"))] \
        + ["-fsyntax-only"]


def self_test(clang_cl=None, verbose=True, db_path=None, repo_root="."):
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        if verbose:
            print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                                   ("  -- " + detail) if detail and not ok else ""))

    repo_root = os.path.abspath(repo_root)
    db_path = db_path or os.path.join(repo_root, "build", "vs2022-asan",
                                      "compile_commands.json")
    clang_cl = clang_cl or os.environ.get("PDFE_CLANG_CL") or DEFAULT_CLANG_CL
    check("clang-cl is available for the compiler oracle",
          os.path.exists(clang_cl), clang_cl)
    check("MSVC/Windows SDK include paths resolved",
          bool(msvc_include_args(verbose=True)),
          "vcvars64.bat produced no usable INCLUDE")

    std_args = stdlib_args_from_db(db_path, repo_root)
    check("a compile database is available for the standard-library checks",
          std_args is not None,
          "no usable entries in %s -- run: cmake --preset windows-vs2022-asan"
          % db_path)
    if std_args is None:
        std_args = ["--driver-mode=cl", "-TP", "-std:c++20", "-fsyntax-only"] \
            + msvc_include_args()

    plain_args = (["--driver-mode=cl", "-TP", "-std:c++20", "-fsyntax-only"]
                  + msvc_include_args())

    tmpdir = tempfile.mkdtemp(prefix="fz_try_raii_")
    try:
        # ---- 1. the compiler oracle, on real standard-library types --------
        fpath = os.path.join(tmpdir, "fz_try_raii_fixture.cpp")
        with io.open(fpath, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(_FIXTURE)
        tu = ci.Index.create().parse(
            fpath, args=std_args,
            options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        fatals = [d.spelling for d in tu.diagnostics
                  if d.severity >= ci.Diagnostic.Fatal]
        check("fixture parses without fatal diagnostics", not fatals,
              "; ".join(fatals[:3]))

        byfun = {}
        for c in tu.cursor.walk_preorder():
            if c.kind != CursorKind.FUNCTION_DECL:
                continue
            if c.spelling not in ("trivial_probe", "nontrivial_probe"):
                continue
            for v in c.walk_preorder():
                if v.kind == CursorKind.VAR_DECL:
                    byfun.setdefault(c.spelling, {})[v.spelling] = v.type
        tp = byfun.get("trivial_probe", {})
        ntp = byfun.get("nontrivial_probe", {})
        check("fixture yielded both probe functions", bool(tp) and bool(ntp),
              "trivial=%d nontrivial=%d" % (len(tp), len(ntp)))

        spellings = set()
        for d in (tp, ntp):
            for t in d.values():
                if quick_class(t) != TRIVIAL:
                    spellings.add(t.get_canonical().spelling)
        verdicts = classify_types_with_compiler(fpath, std_args, spellings, clang_cl)

        def classify(t):
            q = quick_class(t)
            if q:
                return q
            return verdicts.get(t.get_canonical().spelling, UNKNOWN)

        for n in sorted(_TRIVIAL_EXPECTED):
            check("trivial: %s classified TRIVIAL" % n,
                  n in tp and classify(tp[n]) == TRIVIAL,
                  "got %r" % (classify(tp[n]) if n in tp else "absent",))
        check("trivial: pointer to a type with a virtual dtor is TRIVIAL",
              "pe" in ntp and classify(ntp["pe"]) == TRIVIAL)
        for n in sorted(_NONTRIVIAL_EXPECTED):
            check("nontrivial: %s classified NONTRIVIAL" % n,
                  n in ntp and classify(ntp[n]) == NONTRIVIAL,
                  "got %r" % (classify(ntp[n]) if n in ntp else "absent",))

        check("libclang alone would have got std::string WRONG "
              "(this is why the compiler is the oracle)",
              "a" in ntp and ast_destructor_class(ntp["a"]) != NONTRIVIAL,
              "ast_destructor_class now agrees; revisit the docstring")

        # ---- 2. region discovery, independent of any header ----------------
        rpath = os.path.join(tmpdir, "regions.cpp")
        with io.open(rpath, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(_REGION_FIXTURE)
        rtu = ci.Index.create().parse(
            rpath, args=plain_args,
            options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        rfatal = [d.spelling for d in rtu.diagnostics
                  if d.severity >= ci.Diagnostic.Error]
        check("region fixture compiles cleanly", not rfatal, "; ".join(rfatal[:3]))
        rroot = _norm(tmpdir)

        def in_tree(p):
            return bool(p) and p.startswith(rroot + os.sep)

        tries = fz_try_regions(rtu, in_tree)
        check("exactly one fz_try region (the commented one is not counted)",
              len(tries) == 1, "got %d" % len(tries))
        loose = execute_lambda_regions(rtu, in_tree, strict_class=False)
        check("three execute-shaped lambda regions without the class filter",
              len(loose) == 3, "got %d" % len(loose))
        strict = execute_lambda_regions(rtu, in_tree, strict_class=True)
        check("two regions once the callee must be MuPdfContext",
              len(strict) == 2, "got %d" % len(strict))

        entry = {"file": rpath, "arguments": ["clang-cl"] + plain_args + [rpath]}
        res = scan_tu(entry, tmpdir, clang_cl)
        names = sorted(h["name"] for h in res["hazards"])
        check("hazards are exactly the three inside MuPdfContext regions",
              names == ["in_ret", "inside_lambda", "inside_try"],
              "got %r" % (names,))
        check("a local outside every region is not reported",
              "outside_ok" not in names)
        check("a local named only inside a comment is not reported",
              "commented_out" not in names)
        check("a lambda passed to an unrelated execute() is not reported",
              "unrelated_execute" not in names)
        check("the scan reported no parse error for the region fixture",
              not res["error"], res["error"][:120])

        # ---- 3. an UNKNOWN verdict is never folded into 'fine' -------------
        unknown = classify_types_with_compiler(
            fpath, std_args, {"ThisTypeDoesNotExist"}, clang_cl)
        check("an unnameable type is reported UNKNOWN, not trivial",
              unknown.get("ThisTypeDoesNotExist") == UNKNOWN,
              "got %r" % (unknown.get("ThisTypeDoesNotExist"),))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    passed = sum(1 for _, ok, _ in checks if ok)
    print("self-test: %d/%d checks passed" % (passed, len(checks)))
    return passed == len(checks)


# --------------------------------------------------------------------------
# baseline / gate mode
# --------------------------------------------------------------------------

def hazard_key(h):
    return "%s:%s:%s:%s" % (h["file"], h["hazard"], h["name"], h["type"])


def compare_to_baseline(result, baseline_path):
    with io.open(baseline_path, encoding="utf-8") as fh:
        baseline = json.load(fh)
    allowed = {hazard_key(h) for h in baseline.get("accepted", [])}
    present = {hazard_key(h) for h in result["hazards"]}
    new = [h for h in result["hazards"] if hazard_key(h) not in allowed]
    still = [h for h in result["hazards"] if hazard_key(h) in allowed]
    gone = sorted(allowed - present)
    return new, still, gone


# --------------------------------------------------------------------------
# compile-database coverage (GitHub #187)
#
# Broken out of main() so each decision can be unit-tested BOTH WAYS without a
# clang toolchain: a guard that has only ever been seen to redden is as
# worthless as one that can only ever green.
# --------------------------------------------------------------------------

def db_file_set(db_entries):
    """Normalized set of the source paths a compile database actually covers."""
    out = set()
    for e in db_entries:
        try:
            out.add(_norm(e["file"]))
        except Exception:
            pass
    return out


def unresolved_named_files(named, db_files, repo_root):
    """Of the files the caller named with --file, which are NOT in the database.

    Accepts either a repo-relative or an absolute spelling, because both
    appear in real invocations.
    """
    return sorted(
        f for f in named
        if _norm(os.path.join(repo_root, f)) not in db_files
        and _norm(f) not in db_files)


def sources_on_disk(repo_root, subdirs):
    """Every C++ translation unit under subdirs, as it exists in the tree now."""
    found = []
    for sd in subdirs:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(repo_root, sd)):
            for fn in filenames:
                if fn.endswith((".cpp", ".cc", ".cxx")):
                    found.append(os.path.join(dirpath, fn))
    return found


def uncovered_sources(on_disk, db_files, repo_root):
    """Which on-disk translation units the compile database does not cover."""
    return sorted(
        os.path.relpath(p, repo_root).replace("\\", "/")
        for p in on_disk if _norm(p) not in db_files)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--compile-commands",
                    default="build/vs2022-asan/compile_commands.json")
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--subdir", action="append", default=None,
                    help="repo subdirectory to scan (default: src)")
    ap.add_argument("--file", action="append", default=None,
                    help="limit the scan to these source files")
    ap.add_argument("--jobs", type=int, default=None)
    ap.add_argument("--json", default=None, help="write the full result here")
    ap.add_argument("--baseline", default=None,
                    help="compare against this baseline and fail on new hazards")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--clang-cl", default=None)
    ap.add_argument("--progress", action="store_true")
    ap.add_argument("--allow-parse-errors", action="store_true",
                    help="do not fail when some translation units do not parse, "
                         "or when some source file under --subdir is absent "
                         "from the compile database (reduced coverage is then "
                         "PRINTED, never hidden)")
    args = ap.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root)
    db = args.compile_commands
    if not os.path.isabs(db):
        db = os.path.join(repo_root, db)

    if args.self_test:
        return 0 if self_test(args.clang_cl, db_path=db, repo_root=repo_root) else 1

    if not os.path.exists(db):
        sys.stderr.write(
            "fz_try_raii_scan: no compile database at %s\n"
            "  generate one with:  cmake --preset windows-vs2022-asan\n" % db)
        return 2

    subdirs = tuple(args.subdir) if args.subdir else ("src",)

    # ---------------------------------------------------------------------
    # "COULD NOT CHECK" IS NOT "CLEAN" (GitHub #187).
    #
    # Until 2026-09-13 this tool printed `translation units : 0 / hazards : 0`
    # and exited 0 whenever its scan universe came out empty -- most easily
    # reproduced with
    #
    #     --file src/measure/MeasureDictReader.cpp
    #
    # a file that exists on disk but was absent from a stale compile database.
    # A reader, and every CI step that only inspects the exit code, cannot tell
    # that from a genuinely clean sweep. A hazard of exactly the shape this
    # tool exists to find shipped in p84 phase 6 behind that pass.
    #
    # A file missing from the compile database is NOT a parse error, so the
    # parse-error gate below would never mention it. The database is a
    # snapshot of what existed when CMake last configured; every .cpp added
    # since is simply invisible. This is the same defect and the same remedy
    # already carried by page_load_guard_scan.py (GitHub #176) -- enumerate
    # the tree, compare, and fail loudly. Reduced coverage is an explicit
    # opt-in (--allow-parse-errors), exactly as a parse error already is.
    # ---------------------------------------------------------------------
    db_entries = load_compile_commands(db, repo_root, subdirs)
    db_files = db_file_set(db_entries)

    if args.file:
        # A targeted scan. Every file the caller NAMED must resolve to a
        # database entry, or the scan silently covers less than it was asked
        # to -- possibly nothing at all.
        unresolved = unresolved_named_files(args.file, db_files, repo_root)
        if unresolved:
            sys.stderr.write(
                "fz_try_raii_scan: %d file(s) named with --file are absent from\n"
                "  the compile database and were therefore NOT SCANNED:\n%s\n"
                "  An unscanned file reports zero hazards and is indistinguishable\n"
                "  from a clean one, so this is a hard failure -- 'could not check'\n"
                "  is not 'checked and clean'. Re-configure CMake to refresh the\n"
                "  database:  cmake --preset windows-vs2022-asan\n"
                % (len(unresolved), "".join("    %s\n" % u for u in unresolved)))
            return 4
    else:
        on_disk = sources_on_disk(repo_root, subdirs)
        uncovered = uncovered_sources(on_disk, db_files, repo_root)

        print("=== compile-database coverage ===")
        print("%s on disk            : %d" % (subdirs, len(on_disk)))
        print("present in compile db : %d" % (len(on_disk) - len(uncovered)))
        print("NOT in compile db     : %d" % len(uncovered))
        for p in uncovered:
            print("    %s" % p)
        print()

        if uncovered and not args.allow_parse_errors:
            sys.stderr.write(
                "fz_try_raii_scan: %d source file(s) under %s are absent from the\n"
                "  compile database and were therefore NOT SCANNED. This is a hard\n"
                "  failure: an unscanned file reports zero hazards and is\n"
                "  indistinguishable from a clean one. Re-configure CMake to refresh\n"
                "  the database (cmake --preset windows-vs2022-asan), or pass\n"
                "  --allow-parse-errors to accept the stated reduced coverage.\n"
                % (len(uncovered), subdirs))
            return 4

    result = run_scan(db, repo_root, args.jobs, subdirs, args.file,
                      args.clang_cl, args.progress)

    if result["translation_units"] == 0:
        # Belt and braces. The two checks above cover the known ways the
        # universe empties out; this catches the ones nobody has thought of
        # yet (an empty database, a --subdir that matches nothing, a filter
        # that excluded everything). There is deliberately NO opt-out: a sweep
        # that scanned zero translation units has produced no evidence at all,
        # and no flag should be able to dress that up as a pass.
        sys.stderr.write(
            "fz_try_raii_scan: scanned 0 translation units -- nothing was\n"
            "  checked. 'Could not check' is not 'checked and clean', so this\n"
            "  is a hard failure. Verify --compile-commands (%s), --subdir %s\n"
            "  and --file actually select source in this tree.\n"
            % (db, list(subdirs)))
        return 4

    print("translation units : %d" % result["translation_units"])
    print("guarded regions   : %d  %s" % (
        result["regions"],
        " ".join("%s=%d" % kv for kv in sorted(result["region_kinds"].items()))))
    print("hazards           : %d  %s" % (
        result["hazard_count"],
        " ".join("%s=%d" % kv for kv in sorted(result["hazard_kinds"].items()))))
    for h in result["hazards"]:
        print("  %s:%d:%d  %s  %s %s  [in %s at line %d]" % (
            h["file"], h["line"], h["column"], h["hazard"], h["type"],
            h["name"], h["region"], h["region_line"]))
    # A zero here means "no hazard OF THE SHAPES BELOW", and the shapes below
    # are a lower bound. Printing that next to the number is the difference
    # between a reader taking this as a clean bill of health and taking it as
    # what it is. The docstring says the same at length; nobody reading a CI
    # log opens the docstring.
    print()
    print("KNOWN LIMITATIONS -- this count is a LOWER BOUND, not a clean bill:")
    print("  * INTERPROCEDURALLY BLIND. A hazard one call deep -- a callee that")
    print("    constructs an RAII object and can throw -- is out of scope by")
    print("    design; it is undecidable from one translation unit.")
    print("  * Raw owning pointers released by hand (fz_buffer* + fz_drop_buffer)")
    print("    are not VarDecls with non-trivial destructors, so they are invisible")
    print("    here. Two such sites were found by REVIEW in phase-68, not by this.")
    print("  * Implicit throws (std::bad_alloc from v.assign(...)) are not matched;")
    print("    only an explicit throw-expression is.")
    print("  Manual review still owns those three classes.")
    print()

    if result["errors"]:
        print("parse errors      : %d" % len(result["errors"]))
        for e in result["errors"][:10]:
            print("    %s: %s" % (e["file"], e["error"][:160]))

    if args.json:
        with io.open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
            fh.write("\n")

    if result["errors"] and not args.allow_parse_errors:
        sys.stderr.write(
            "fz_try_raii_scan: %d translation unit(s) failed to parse.\n"
            "  A sweep that parsed nothing reports zero hazards and looks\n"
            "  identical to a clean tree, so this is a hard failure. Pass\n"
            "  --allow-parse-errors only if you have read the errors above\n"
            "  and accept the reduced coverage.\n" % len(result["errors"]))
        return 3

    if args.update_baseline and args.baseline:
        with io.open(args.baseline, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"accepted": result["hazards"],
                       "translation_units": result["translation_units"],
                       "regions": result["regions"]},
                      fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("baseline written: %s" % args.baseline)
        return 0

    if args.baseline:
        new, still, gone = compare_to_baseline(result, args.baseline)
        print("baseline: %d accepted still present, %d new, %d resolved"
              % (len(still), len(new), len(gone)))
        for h in new:
            print("  NEW HAZARD %s:%d %s %s %s"
                  % (h["file"], h["line"], h["hazard"], h["type"], h["name"]))
        if new:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
