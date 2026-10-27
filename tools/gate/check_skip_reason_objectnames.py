#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fail when a UI-test skip reason asserts the absence of an objectName that
``src/`` actually defines.

GitHub #209, ask 3. This class of defect has now been recorded three times:

  * GitHub #178 (CLOSED) — two stale skip reasons in
    ``tests/ui_automation/test_canvas_coords_p2.py`` describing work that had
    been done two months earlier.
  * GitHub #198 (OPEN) — four call sites hard-coding a stale ``pdfCanvas``
    UIA path and reporting the result as "canvas absent".
  * GitHub #209 — five ``*_phase_post.py`` files, >=7 cases. Among them
    ``pytest.skip("invisibleCheck not in this build")``, emitted against a
    build in which ``src/ui/SignDialog.cpp`` had said
    ``invisibleCheck_->setObjectName("invisibleCheckBox")`` for four months.

A skip is GREEN. A suite that skips on a false premise reports success for
coverage it does not have, and the premise is recorded *inside the test*,
where nothing re-checks it — until this guard.

WHAT IS CHECKED
---------------
Every string that is passed as a SKIP REASON in ``tests/ui_automation/**/*.py``
— the argument of ``pytest.skip`` / ``pytest.xfail`` / ``pytest.importorskip``
and the ``reason=`` of ``pytest.mark.skip`` / ``skipif`` / ``xfail``, i.e. only
the surfaces that report GREEN (see REASON_CALLS for why ``pytest.fail`` is
out of scope) — is matched against a small set of ABSENCE CLAIMS:

    <ident> not in this build        <ident> not implemented
    <ident> absent from ...          <ident> not present
    <ident> does not exist           <ident> is missing
                                     has no <ident>

``<ident>`` may be bare, `backticked`, 'single-quoted' or "double-quoted".
The claim is false — and the file fails — when ``src/`` either

  * sets that exact name with ``setObjectName("<ident>")``, or
  * sets a RENAMED form of it (``rename_neighbours``). Both #209 sign-dialog
    reasons were of the second kind: ``invisibleCheck`` is genuinely not an
    objectName, because the commit the reason was describing had already
    renamed the widget to ``invisibleCheckBox``. Exact membership alone
    reports such a reason as truthful and misses the whole instance.

Reasons built with an f-string, or bound to a local before being passed, are
resolved too (``parametrize_literals`` / ``_scope_string_vars``): #209's
seventh group wrote ``_reach = f"palette action {action_id} absent from UIA
tree"``, which has neither the identifier nor the call-argument shape a naive
scanner looks for.

Reasons are read through ``ast``, so ordinary ``#`` comments and docstrings
that DISCUSS a historical false skip (as several of the #209 fixes do, by
quoting the text they replaced) are not matched. Only a live skip reason is.

SELF-MATCH EXCLUSION
--------------------
This module's own source, and its self-test, contain the exact phrases and
identifiers they search for — ``invisibleCheck not in this build`` appears
literally a few lines above. Three guards in this repository have been
reddened by exactly that (``lint_unc_gate`` among them). Two defences:

  1. The scan root is ``tests/ui_automation``; ``tools/gate/`` is not under it.
  2. ``_is_excluded()`` additionally rejects this file and its self-test by
     resolved path, and ``--selftest-exclusion`` proves the exclusion is load
     bearing by pointing the scanner AT ``tools/gate`` and showing it still
     reports zero findings.

Exit codes: 0 = clean, 1 = false skip reasons found, 2 = usage/IO error.
"""
from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, NamedTuple, Set

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


REPO_ROOT = Path(__file__).resolve().parents[2]

#: Calls whose string arguments are treated as skip reasons.
#:
#: SCOPE, stated deliberately: only surfaces that report GREEN. ``pytest.fail``
#: and ``record_matrix_result(..., "blocked"/"red", ...)`` are excluded even
#: though they carry the same prose, because the thesis of GitHub #209 is
#: precisely that "a skip is green": a FAIL that says "atlasModeCombo absent
#: from UIA tree" costs nothing if the wording is imprecise — the suite is red
#: either way and someone must look. A SKIP that says it is a silent claim of
#: coverage. Including the fail surface would add four findings today
#: (test_canvas_click_subsystems.py:554,577 and test_ui_alt_text.py:116,384 —
#: all WIN-45 recorded-non-pass sites, one of which explicitly documents the
#: GitHub #198 fix) for no green-masking risk, and a guard that cries wolf
#: gets ignored.
REASON_CALLS = {
    "skip", "xfail", "importorskip",
}

#: Decorator factories whose ``reason=`` keyword is a skip reason.
REASON_DECORATORS = {"skip", "skipif", "xfail"}

#: ``setObjectName("x")`` / ``setObjectName(QStringLiteral("x"))`` /
#: ``setObjectName(QLatin1String("x"))``.
_OBJECT_NAME_RE = re.compile(
    r'setObjectName\s*\(\s*(?:QStringLiteral|QLatin1String|QString::fromLatin1)?'
    r'\s*\(?\s*"([A-Za-z_][A-Za-z0-9_]*)"'
)

#: An identifier, optionally wrapped in backticks or quotes.
_IDENT = r"[`'\"]?(?P<ident>[A-Za-z_][A-Za-z0-9_]*)[`'\"]?"

#: Phrases that assert the identifier immediately BEFORE them is absent.
_TRAILING_PHRASES = [
    r"not\s+in\s+this\s+build",
    r"absent\s+from",
    r"not\s+implemented",
    r"not\s+present",
    r"does\s+not\s+exist",
    r"doesn['’]t\s+exist",
    r"is\s+missing",
]

# DELIBERATELY NOT A TRIGGER: "<ident> not in UIA tree".
#
# That is a RUNTIME REACHABILITY observation, not a claim about what src/
# defines — a widget can be absent from the UIA tree while being perfectly
# well defined (hidden dock, collapsed panel, un-shown dialog, or the Qt UIA
# bridge simply not exposing it). Twelve such reasons exist today
# (test_ui_search_replace.py, test_canvas_click_subsystems.py,
# test_broader_phase_post.py) and every one of them is honest: they record a
# non-pass and FAIL rather than skipping to green. Firing on them would make
# this guard noisy, and a noisy guard gets ignored — `noisy_guard_masks_
# everything` in this repo's own history is a path-hygiene check that matched
# 365 files and hid four real failures for five commits.
#
# The class GitHub #209 is about is narrower and mechanically decidable: a
# reason that says the thing DOES NOT EXIST IN THE PRODUCT, about a name the
# product defines.

ABSENCE_PATTERNS = [
    re.compile(_IDENT + r"\s+(?:is\s+|was\s+|are\s+)?(?:" + p + r")",
               re.IGNORECASE)
    for p in _TRAILING_PHRASES
] + [
    # "... has no <ident>" / "... contains no <ident>"
    re.compile(r"(?:has|have|contains)\s+no\s+" + _IDENT, re.IGNORECASE),
    # "no <ident> in this build"
    re.compile(r"\bno\s+" + _IDENT + r"\s+in\s+this\s+build", re.IGNORECASE),
]

#: Words that look like identifiers but are never objectNames; cheap noise cut
#: before the (authoritative) src/ membership test.
_STOPWORDS = {"a", "an", "the", "it", "this", "that", "menu", "dialog",
              "action", "button", "widget", "wiring", "such", "any"}


class Finding(NamedTuple):
    path: Path
    line: int
    ident: str
    reason: str
    defined_at: str

    def render(self) -> str:
        rel = self.path.as_posix()
        return (
            f"{rel}:{self.line}: skip reason claims {self.ident!r} is absent, "
            f"but src/ defines it: {self.defined_at}\n"
            f"    reason: {self.reason.strip()[:240]!r}"
        )


def collect_src_object_names(src_root: Path) -> Dict[str, str]:
    """Map objectName -> "<file>:<line>" for every setObjectName literal."""
    found: Dict[str, str] = {}
    if not src_root.is_dir():
        return found
    for path in sorted(src_root.rglob("*")):
        if path.suffix.lower() not in {".cpp", ".h", ".hpp", ".cc", ".cxx"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            for m in _OBJECT_NAME_RE.finditer(line):
                name = m.group(1)
                found.setdefault(
                    name,
                    f"{path.relative_to(src_root.parent).as_posix()}:{lineno}")
    return found


def _split_camel(name: str) -> List[str]:
    """``invisibleCheckBox`` -> ``['invisible', 'check', 'box']``."""
    return [t.lower() for t in
            re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+", name) if t]


def _is_subsequence(small: List[str], big: List[str]) -> bool:
    it = iter(big)
    return all(tok in it for tok in small)


def rename_neighbours(ident: str, object_names: Dict[str, str]) -> List[str]:
    """Defined objectNames that ``ident`` looks like a STALE RENAME of.

    This is the half of GitHub #209 an exact-match check cannot see, and it is
    the half that actually happened. The recorded reasons were
    ``"invisibleCheck not in this build"`` and
    ``"certificationCombo not in this build"`` — and neither of those strings
    is an objectName in src/, because the very commit the reasons were
    describing had renamed them to ``invisibleCheckBox`` and
    ``certificationLevelCombo`` (src/ui/SignDialog.cpp:174-183 still carries
    the comment recording the rename). An exact-membership guard reports such
    a reason as truthful: the name really is absent. It is the WIDGET that is
    present, which is what the reader of a skip is being told about.

    A defined name D is treated as the same widget under a new name when D's
    camelCase tokens are a strict superset-in-order of ``ident``'s, sharing
    the first token. ``invisible|check`` vs ``invisible|check|box`` and
    ``certification|combo`` vs ``certification|level|combo`` both qualify;
    ``okButton`` vs ``cancelButton`` does not (first token differs), and
    ``pfxPathEdit`` vs ``tsaUrlEdit`` does not (not a subsequence).
    """
    ident_tokens = _split_camel(ident)
    if len(ident_tokens) < 2:
        # One token is too little signal — "combo", "dialog", "canvas" would
        # match half the tree and the guard would become noise.
        return []
    out = []
    for defined in object_names:
        if defined == ident:
            continue
        d_tokens = _split_camel(defined)
        if len(d_tokens) <= len(ident_tokens):
            continue
        if d_tokens[0] != ident_tokens[0]:
            continue
        if _is_subsequence(ident_tokens, d_tokens):
            out.append(defined)
    return sorted(out)


def parametrize_literals(tree: ast.AST) -> List[str]:
    """Every string literal appearing in a ``@pytest.mark.parametrize``
    argvalues list in this module.

    GitHub #209's seventh group built its reason with an f-string —
    ``f"palette action {action_id} absent from UIA tree"`` — so the
    identifier is not in any literal. It IS in the parametrize list that
    supplies ``action_id``. Substituting those candidates back into the hole
    recovers the claim the reader actually sees.
    """
    vals: List[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        if _callee_name(node.func) != "parametrize":
            continue
        for arg in node.args[1:]:
            for sub in ast.walk(arg):
                if isinstance(sub, ast.Constant) and isinstance(sub.value, str):
                    vals.append(sub.value)
    return vals


def _literal_parts(node: ast.AST, fillers: Iterable[str] = ()) -> Iterable[str]:
    """Yield the literal text of a str constant, or of an f-string.

    For an f-string the literal segments are joined with a placeholder, and —
    when *fillers* is non-empty — one variant per filler is also produced with
    every hole set to that filler. See :func:`parametrize_literals`.
    """
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        yield node.value
    elif isinstance(node, ast.JoinedStr):
        segments = [v.value if isinstance(v, ast.Constant)
                    and isinstance(v.value, str) else None
                    for v in node.values]
        yield "".join(s for s in segments if s is not None)
        holes = sum(1 for s in segments if s is None)
        if holes:
            for filler in fillers:
                yield "".join(s if s is not None else filler
                              for s in segments)
    elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        yield from _literal_parts(node.left, fillers)
        yield from _literal_parts(node.right, fillers)


def _callee_name(func: ast.AST) -> str:
    if isinstance(func, ast.Name):
        return func.id
    if isinstance(func, ast.Attribute):
        return func.attr
    return ""


def _scope_string_vars(scope: ast.AST, fillers: List[str]
                       ) -> Dict[str, List[str]]:
    """Map ``name -> [literal texts]`` for string assignments in one scope.

    GitHub #209's seventh group did not pass its reason inline — it built one
    and bound it first::

        _reach = f"palette action {action_id} absent from UIA tree"
        record_matrix_result(current_test_id(), "blocked", _reach)

    so an extractor that only reads call ARGUMENTS sees a bare ``Name`` and
    finds nothing. Resolution is deliberately per-function: ``_reach`` is
    rebound in nearly every test in these files, and a module-wide map would
    attribute one function's reason to another's call.
    """
    out: Dict[str, List[str]] = {}
    for node in ast.walk(scope):
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) \
                else [node.target]
            value = node.value
            if value is None:
                continue
            parts = list(_literal_parts(value, fillers))
            if not parts:
                continue
            for t in targets:
                if isinstance(t, ast.Name):
                    out.setdefault(t.id, []).extend(parts)
    return out


def _arg_texts(node: ast.AST, fillers: List[str],
               scope_vars: Dict[str, List[str]]) -> List[str]:
    parts = list(_literal_parts(node, fillers))
    if not parts and isinstance(node, ast.Name):
        parts = list(scope_vars.get(node.id, ()))
    return parts


def extract_reasons(tree: ast.AST) -> List[tuple]:
    """Return ``(lineno, reason_text)`` for every skip-reason string."""
    out: List[tuple] = []
    fillers = parametrize_literals(tree)

    # Nearest enclosing function for every node, so a reason variable is
    # resolved in the scope that bound it.
    enclosing: Dict[int, ast.AST] = {}

    def _mark(node: ast.AST, scope: ast.AST) -> None:
        for child in ast.iter_child_nodes(node):
            child_scope = child if isinstance(
                child, (ast.FunctionDef, ast.AsyncFunctionDef)) else scope
            enclosing[id(child)] = child_scope
            _mark(child, child_scope)

    enclosing[id(tree)] = tree
    _mark(tree, tree)
    scope_cache: Dict[int, Dict[str, List[str]]] = {}

    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        name = _callee_name(node.func)
        if name not in REASON_CALLS and name not in REASON_DECORATORS:
            continue
        scope = enclosing.get(id(node), tree)
        if id(scope) not in scope_cache:
            scope_cache[id(scope)] = _scope_string_vars(scope, fillers)
        scope_vars = scope_cache[id(scope)]

        if name in REASON_CALLS:
            for arg in node.args:
                for part in _arg_texts(arg, fillers, scope_vars):
                    out.append((getattr(arg, "lineno", node.lineno), part))
        for kw in node.keywords:
            if kw.arg in ("reason", "notes", "msg"):
                for part in _arg_texts(kw.value, fillers, scope_vars):
                    out.append(
                        (getattr(kw.value, "lineno", node.lineno), part))
    return out


def claimed_absent(reason: str) -> Set[str]:
    """Identifiers this reason asserts do not exist."""
    idents: Set[str] = set()
    for pat in ABSENCE_PATTERNS:
        for m in pat.finditer(reason):
            ident = m.group("ident")
            if not ident or ident.lower() in _STOPWORDS:
                continue
            idents.add(ident)
    return idents


def _is_excluded(path: Path) -> bool:
    """Self-match defence — see the module docstring.

    This file and its self-test quote the very reasons they hunt for, so they
    are rejected by resolved path regardless of where a caller points --root.
    """
    try:
        resolved = path.resolve()
    except OSError:
        return False
    here = Path(__file__).resolve()
    selftest = here.with_name("test_check_skip_reason_objectnames.py")
    return resolved in (here, selftest)


def scan(root: Path, object_names: Dict[str, str],
         bypass_exclusion: bool = False) -> List[Finding]:
    findings: List[Finding] = []
    if not root.exists():
        return findings
    paths = [root] if root.is_file() else sorted(root.rglob("*.py"))
    for path in paths:
        if not bypass_exclusion and _is_excluded(path):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
            tree = ast.parse(text, filename=str(path))
        except (OSError, SyntaxError):
            continue
        for lineno, reason in extract_reasons(tree):
            for ident in sorted(claimed_absent(reason)):
                try:
                    rel = path.relative_to(REPO_ROOT)
                except ValueError:
                    rel = path
                if ident in object_names:
                    findings.append(Finding(rel, lineno, ident, reason,
                                            object_names[ident]))
                    continue
                neighbours = rename_neighbours(ident, object_names)
                if neighbours:
                    where = ", ".join(
                        f"{n} ({object_names[n]})" for n in neighbours[:3])
                    findings.append(Finding(
                        rel, lineno, ident, reason,
                        f"RENAMED — src/ defines {where}"))
    return findings


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default="tests/ui_automation",
                    help="directory (or file) of UI tests to scan")
    ap.add_argument("--src", default="src",
                    help="directory scanned for setObjectName literals")
    ap.add_argument("--repo", default=str(REPO_ROOT),
                    help="repository root that --root/--src are relative to")
    ap.add_argument("--selftest-exclusion", action="store_true",
                    help="point the scanner at tools/gate (which contains this "
                         "guard's own decoy strings) and require zero findings")
    args = ap.parse_args(argv)

    repo = Path(args.repo).resolve()
    src_root = (repo / args.src) if not Path(args.src).is_absolute() \
        else Path(args.src)
    object_names = collect_src_object_names(src_root)
    if not object_names:
        print(f"ERROR: no setObjectName literals found under {src_root} — "
              f"the guard would pass vacuously, so this is an error.",
              file=sys.stderr)
        return 2
    print(f"[skip-reason-objectnames] {len(object_names)} objectNames defined "
          f"under {src_root.name}/")

    if args.selftest_exclusion:
        gate_dir = Path(__file__).resolve().parent
        # Direction 1 — the exclusion must be LOAD BEARING. With it bypassed,
        # the decoy skip reasons in this guard's own self-test must be caught.
        # If they are not, "0 findings with the exclusion on" would prove
        # nothing at all (a one-against-one that is really zero-against-zero).
        decoys = scan(gate_dir, object_names, bypass_exclusion=True)
        if not decoys:
            print("SELF-MATCH PROOF VACUOUS — with the exclusion bypassed the "
                  "scanner found NO decoy in tools/gate, so the exclusion is "
                  "not demonstrably doing anything. Expected the decoy skip "
                  "reasons in test_check_skip_reason_objectnames.py.",
                  file=sys.stderr)
            return 1
        print(f"[skip-reason-objectnames] exclusion is load bearing: "
              f"{len(decoys)} decoy reason(s) in {gate_dir.name}/ are caught "
              f"when the exclusion is bypassed "
              f"({', '.join(sorted({d.ident for d in decoys}))})")
        # Direction 2 — with the exclusion on, those same files are silent.
        leaked = scan(gate_dir, object_names)
        if leaked:
            print("SELF-MATCH EXCLUSION FAILED — the guard flagged its own "
                  "decoy strings:", file=sys.stderr)
            for f in leaked:
                print("  " + f.render(), file=sys.stderr)
            return 1
        print(f"[skip-reason-objectnames] self-match exclusion OK: scanning "
              f"{gate_dir.name}/ (which contains the decoys) yields 0 findings")
        return 0

    root = (repo / args.root) if not Path(args.root).is_absolute() \
        else Path(args.root)
    findings = scan(root, object_names)
    if findings:
        print(f"\nFALSE SKIP REASONS: {len(findings)} skip reason(s) assert "
              f"the absence of an objectName that src/ defines.\n",
              file=sys.stderr)
        for f in findings:
            print(f.render(), file=sys.stderr)
        print("\nFix the test (use the real objectName, and make the case "
              "assert the behaviour) rather than the message. See GitHub "
              "#209.", file=sys.stderr)
        return 1
    print(f"[skip-reason-objectnames] OK — no false absence claims under "
          f"{args.root}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
