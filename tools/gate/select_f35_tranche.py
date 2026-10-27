#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Mechanical selector for the F35 first tranche of WIN-45 reachability sites.

Why a selector script exists
----------------------------
`reachability_skip_baseline.json` ratcheted **468** sites carrying the WIN-45
statement shape (a lookup guard whose failure branch exits without failing the
test) when F29 installed the detector; it stands at **448** after the F35
tranche, and `count` in that file is always the current figure.  Converting all
468 in one phase was neither reviewable nor honest, so F35 converts a **bounded
tranche chosen by a written rule** rather than by hand.

This module IS that rule.  It is executable so that the tranche is derived, not
asserted: `--list` prints exactly the sites the rule selects, and the count in
`phase-35-dispositions.md` is whatever this script prints.

Rule R35
--------
A baseline site is in the F35 tranche iff ALL of:

  S1  FILE SCOPE -- the site is in one of `--files` (F35 used the single file
      `tests/test_ui_preferences_chrome.py`).

  S2  EXIT KIND IS `skip` -- the detector classified the branch's early exit as
      a `pytest.skip` / `raise SkipTest`, not as `return` or `continue`.
      `return`/`continue` sites are excluded on purpose: in this tree they are
      overwhelmingly loop or helper control flow that the detector cannot
      distinguish from a test-level bail-out (e.g. a `continue` belonging to an
      inner `for` inside the guarded block).  Converting one mechanically would
      change real control flow, so they need per-site judgement and are left for
      a later tranche.

  S3  PROBES THE LIVE SESSION -- at least one call in the guard condition's
      provenance (following local assignments, exactly as the detector does)
      either is a method on, or receives as an argument, the **live-UI session
      object**: the name bound by a live-UI session factory -- either
      `with <...session...>(...) as NAME:` or `NAME = PDFESession(...)` /
      `NAME = <...session...>(...).__enter__()` -- or a parameter named
      `s`/`session`.

S3 is the discriminator that keeps legitimate external-tool gates OUT of the
tranche.  An external-tool or optional-dependency gate answers a question whose
answer does not depend on the running application --

    acrobat = _find_acrobat()      # tests/test_print_acrobat_interop.py
    if acrobat is None: pytest.skip(...)

    if not _has_pikepdf(): pytest.skip(...)
    if not p.exists(): pytest.skip(...)

-- and none of those calls is handed the session.  A WIN-45 site, by contrast,
is a probe *of the product under test through a live session*:

    if not _open_customize_toolbars_dialog(s): pytest.skip(...)
    combo = _wait_for_dialog_widget(s, "toolbarCombo", timeout=3.0)
    if combo is None: pytest.skip(...)

Those, and only those, are what "the test could not reach the control" means.

Usage
-----
    python tools/gate/select_f35_tranche.py --list
    python tools/gate/select_f35_tranche.py --list --files tests/test_ui_forms.py
    python tools/gate/select_f35_tranche.py --json
    python tools/gate/select_f35_tranche.py --audit-excluded   # whole-tree S3 audit

Exit codes: 0 = ok, 2 = usage/parse error.
"""

from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_reachability_skip_shape as detector  # noqa: E402

#: Default file scope for the F35 tranche (S1).
DEFAULT_FILES = ("tests/test_ui_preferences_chrome.py",)

#: Parameter names that denote a live-UI session in this tree's helpers.
SESSION_PARAM_NAMES = frozenset({"s", "session"})

#: Substrings that mark a call as a live-UI session factory.
SESSION_FACTORY_MARKERS = ("session", "pdfe_app", "launch_pdfe")


def _is_session_factory(value: ast.expr) -> bool:
    """True when `value` evaluates to a live-UI session object.

    Covers both spellings this tree uses: the context-manager form
    (`with _pdfe_session_dirty_safe(...) as s:`) and the explicit-enter form
    (`s = PDFESession(pdf_path=...).__enter__()`), which appears wherever a test
    needs the session to outlive a `with` block.
    """
    if isinstance(value, ast.Call):
        called = detector._call_name(value)
        if called and any(m in called.lower() for m in SESSION_FACTORY_MARKERS):
            return True
        # `.__enter__()` / `.start()` applied to a session factory call
        if isinstance(value.func, ast.Attribute):
            return _is_session_factory(value.func.value)
        return False
    dotted = detector._dotted(value)
    return bool(dotted) and any(m in dotted.lower() for m in SESSION_FACTORY_MARKERS)


def _session_names(func: ast.AST) -> Set[str]:
    """Names inside `func` that hold a live-UI session object."""
    names: Set[str] = set()
    if isinstance(func, (ast.FunctionDef, ast.AsyncFunctionDef)):
        args = func.args
        for arg in list(args.posonlyargs) + list(args.args) + list(args.kwonlyargs):
            if arg.arg in SESSION_PARAM_NAMES:
                names.add(arg.arg)
    for node in ast.walk(func):
        targets: List[ast.expr] = []
        value: Optional[ast.expr] = None
        if isinstance(node, ast.withitem) and node.optional_vars is not None:
            targets, value = [node.optional_vars], node.context_expr
        elif isinstance(node, ast.Assign):
            targets, value = list(node.targets), node.value
        elif isinstance(node, (ast.AnnAssign, ast.NamedExpr)) and node.value is not None:
            targets, value = [node.target], node.value
        if value is None or not _is_session_factory(value):
            continue
        for target in targets:
            for sub in ast.walk(target):
                if isinstance(sub, ast.Name):
                    names.add(sub.id)
    return names


def _provenance_calls(
    test: ast.expr,
    index: "detector._AssignmentIndex",
    line: int,
    guard: Optional[ast.stmt] = None,
) -> List[ast.Call]:
    """Every Call reachable from the guard condition through local bindings.

    Mirrors `detector._condition_provenance`'s traversal (same depth limit, same
    binding chase) so the selector and the checker agree about what a guard's
    provenance is.

    GitHub #305, TENTH correction. That claim had become FALSE. This walk used
    `index.latest_before` -- ONE binding, the textually last -- which the ninth
    correction had just replaced in the detector precisely because it is
    unsound. Measured by the p98 phase-3 claim-check gate on the same source:

        if cond:
            w = s.find_window()
        else:
            w = None
        if w is None:
            pytest.skip("not reachable")

    the detector flags it, while this selector saw no session probe and
    classified it `S3:no-session-probe` -- so Rule R35 would have EXCLUDED a
    genuine live-session WIN-45 site from a tranche, backwards from its purpose.
    `test_f35_tranche_controls.py` had no rebinding fixture, so the drift was
    invisible to CI.

    A mirror has to be kept in step deliberately: the two walks now call the
    SAME `reaching()` on the index, and a control in that test file exercises a
    rebound session lookup so a future divergence reddens.
    """
    calls: List[ast.Call] = []
    seen: Set[str] = set()

    def visit(node: ast.AST, depth: int) -> None:
        if depth > 4:
            return
        for sub in ast.walk(node):
            if isinstance(sub, ast.Call):
                calls.append(sub)
            elif isinstance(sub, ast.Name) and sub.id not in seen:
                seen.add(sub.id)
                if guard is not None:
                    sources = index.reaching(sub.id, guard)
                else:
                    sources = index.all_before(sub.id, None)
                for source in sources:
                    if source is not None:
                        visit(source, depth + 1)

    visit(test, 0)
    return calls


def _touches_session(calls: Sequence[ast.Call], sessions: Set[str]) -> Optional[str]:
    """Return the source text of the first session-probing call, else None."""
    for call in calls:
        # method on the session: s.get_title()
        if isinstance(call.func, ast.Attribute):
            root = detector._dotted(call.func.value)
            if root and root.split(".", 1)[0] in sessions:
                return _unparse(call)
        # session handed in: _open_customize_toolbars_dialog(s)
        for arg in list(call.args) + [kw.value for kw in call.keywords]:
            for sub in ast.walk(arg):
                if isinstance(sub, ast.Name) and sub.id in sessions:
                    return _unparse(call)
    return None


def _unparse(node: ast.AST) -> str:
    try:
        text = ast.unparse(node)
    except Exception:  # pragma: no cover - defensive
        return "<?>"
    text = " ".join(text.split())
    return text if len(text) <= 120 else text[:117] + "..."


def classify_file(path: Path, root: Path) -> List[Dict[str, object]]:
    """Classify every detector finding in `path` against S2 and S3."""
    with open(path, "r", encoding="utf-8") as handle:
        source = handle.read()
    rel = path.relative_to(root).as_posix()
    findings = {f.line: f for f in detector.scan_source(source, rel)}
    if not findings:
        return []

    normalised = source.replace("\r\n", "\n").replace("\r", "\n")
    tree = ast.parse(normalised, filename=rel)
    out: List[Dict[str, object]] = []
    for func in ast.walk(tree):
        if not isinstance(func, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        index = detector._AssignmentIndex(func)
        sessions = _session_names(func)
        for node in ast.walk(func):
            if not isinstance(node, ast.If) or node.lineno not in findings:
                continue
            finding = findings[node.lineno]
            if finding.function != func.name:
                continue
            calls = _provenance_calls(node.test, index, node.lineno, guard=node)
            probe = _touches_session(calls, sessions)
            reasons: List[str] = []
            if finding.exit_kind != "skip":
                reasons.append("S2:exit_kind=%s" % finding.exit_kind)
            if probe is None:
                reasons.append("S3:no-session-probe")
            out.append(
                {
                    "site": "%s:%d" % (rel, node.lineno),
                    "file": rel,
                    "line": node.lineno,
                    "function": finding.function,
                    "exit_kind": finding.exit_kind,
                    "condition": finding.condition,
                    "session_probe": probe,
                    "selected": not reasons,
                    "excluded_by": reasons,
                }
            )
    out.sort(key=lambda item: item["line"])
    return out


def _iter_test_files(root: Path) -> List[Path]:
    return [p for p in sorted((root / "tests").rglob("*.py")) if _scannable(p, root / "tests")]


def _scannable(candidate: Path, base: Path) -> bool:
    parts = set(candidate.relative_to(base).parts)
    if parts & {"__pycache__", "_runtime", "_tmp", "build", "out", "vcpkg_installed",
                "external", ".git"}:
        return False
    return candidate.name.startswith("test_") or candidate.name == "conftest.py"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=".")
    parser.add_argument("--files", nargs="+", default=list(DEFAULT_FILES),
                        help="S1 file scope (repo-relative)")
    parser.add_argument("--list", action="store_true", dest="as_list")
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--audit-excluded", action="store_true",
                        help="classify the WHOLE tests tree and print the S3-excluded sites")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    if args.audit_excluded:
        rows: List[Dict[str, object]] = []
        for path in _iter_test_files(root):
            rows.extend(classify_file(path, root))
        excluded = [r for r in rows if "S3:no-session-probe" in r["excluded_by"]]
        sys.stdout.write(
            "%d classified site(s); %d excluded by S3 (no live-session probe)\n"
            % (len(rows), len(excluded))
        )
        if args.as_json:
            sys.stdout.write(json.dumps(rows, indent=1, ensure_ascii=False) + "\n")
        else:
            for row in excluded:
                sys.stdout.write("%s  [%s]  guard: %s\n"
                                 % (row["site"], row["function"], row["condition"]))
        return 0

    rows = []
    for rel in args.files:
        target = root / rel
        if not target.exists():
            sys.stderr.write("no such file: %s\n" % target)
            return 2
        rows.extend(classify_file(target, root))

    selected = [r for r in rows if r["selected"]]
    if args.as_json:
        sys.stdout.write(json.dumps(rows, indent=1, ensure_ascii=False) + "\n")
        return 0

    for row in rows:
        mark = "SELECT" if row["selected"] else "  skip"
        sys.stdout.write(
            "%s %s  [%s]  guard: %s%s\n"
            % (
                mark,
                row["site"],
                row["function"],
                row["condition"],
                "" if row["selected"] else "   -- " + ", ".join(row["excluded_by"]),
            )
        )
    sys.stdout.write("\nRule R35 selects %d of %d detector finding(s) in %s\n"
                     % (len(selected), len(rows), ", ".join(args.files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
