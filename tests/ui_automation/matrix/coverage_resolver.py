# -*- coding: utf-8 -*-
"""Resolve UI-matrix rows to tests that actually exist (gap A1-70 / UI-MATRIX-02).

Why this module exists
======================

``verify_matrix.py`` used to validate only the SHAPE of
``ui_test_matrix.csv``: unique non-empty ``test_id``, enum-valid
``priority``/``status``/``owner_phase``, and the presence of a few required
subsystems. Nothing tied a row marked ``green`` to a test that exists, let
alone one that asserts. A row could claim a passing test that was renamed,
deleted, disabled or never written, and the checker stayed green — which is
precisely the "green row pointing at a stub/deleted/assertion-free test"
failure mode the UI-E2E critique named.

This module adds the missing cross-check. Every row whose status CLAIMS a
result (``green``) must resolve to a real test in the repository, by one of
three evidence kinds:

``covered-by`` (preferred — explicit and unambiguous)
    The row's ``notes`` carry a machine-readable claim
    ``covered-by: tests/test_x.cpp::SuiteName.TestName`` (GoogleTest) or
    ``covered-by: tests/ui_automation/test_x.py::test_fn`` (pytest node id).
    The claim is VERIFIED — the file must exist and actually declare that
    test, otherwise the row is reported unresolved. Prefer this form when
    adding rows; the heuristics below exist for the pre-existing corpus.

``param``
    The ``test_id`` appears as a pytest parametrisation id in the suite's own
    collected node ids (e.g. ``test_p0_menu_item_no_crash[menu_close_doc]``).
    This is the dominant shape: most matrix-driven suites parametrise straight
    off the CSV, so the id never appears as a literal in the source.

``source``
    The ``test_id`` appears literally in a Python or C++ test source (typically
    a ``record_matrix_result("<id>", ...)`` call).

``notes-gtest``
    The row's ``notes`` name a ``test_*.cpp`` that exists AND a GoogleTest test
    name declared in that file. Used by the ``tool=gtest`` rows, whose C++ test
    names deliberately differ from the matrix id.

A row that resolves to a test which is *disabled* (``DISABLED_`` gtest prefix,
or an unconditional ``@pytest.mark.skip``) does NOT count as resolved: a
permanently-skipped test cannot substantiate a green row.

Design: the resolution logic is pure (data in, verdicts out) so it is directly
unit-testable without a repo scan — see ``test_matrix_coverage.py``. The I/O
wrappers at the bottom build the same data from the working tree.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[3]

#: Statuses that CLAIM a passing result and therefore must resolve to a test.
CLAIMING_STATUSES = frozenset({"green"})

#: Statuses that are documented non-results — exempt from resolution, but
#: reported so they can never be mistaken for coverage.
NON_RESULT_STATUSES = frozenset({"wont-fix", "skip", "xfail", "planned",
                                 "blocked", "red", "no-run"})

#: Directories excluded from the "test source" corpus: the matrix directory
#: itself (every id appears in the CSV — that would resolve every row to
#: itself) and the runtime scratch tree (copies of the CSV).
_EXCLUDED_DIR_PARTS = ("__pycache__", "/_runtime/", "/matrix/", "/_tmp/",
                       "/docs/", "/fixtures/")

_GTEST_DECL_RE = re.compile(
    r"\bTEST(?:_F|_P)?\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\)")

_PY_DEF_RE = re.compile(r"^\s*def\s+(test_[A-Za-z0-9_]*)\s*\(", re.M)

_CPP_FILE_IN_NOTES_RE = re.compile(r"\b(test_[A-Za-z0-9_]+\.cpp)\b")
#: ``covered-by: <path>::<target>`` — terminated by ';' or end of line.
_COVERED_BY_RE = re.compile(r"covered-by:\s*([^;\n]+)", re.I)
_IDENT_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{3,})\b")


@dataclass
class Resolution:
    """How (or whether) a matrix row was tied to a real test."""
    test_id: str
    status: str
    #: "covered-by" | "param" | "source" | "notes-gtest"
    kind: Optional[str] = None
    evidence: str = ""
    disabled_reason: str = ""

    @property
    def resolved(self) -> bool:
        return self.kind is not None and not self.disabled_reason


@dataclass
class TestIndex:
    """Everything the resolver needs to know about the repo's tests."""
    #: Collected pytest node ids, e.g. "tests/ui_automation/x.py::test_y[id]".
    node_ids: Sequence[str] = field(default_factory=list)
    #: {relative path: file text} for Python/C++ test sources.
    sources: Dict[str, str] = field(default_factory=dict)

    # -- derived lookups -------------------------------------------------
    def __post_init__(self) -> None:
        self._node_blob = "\n".join(self.node_ids)
        self._cpp_tests: Dict[str, Dict[str, str]] = {}
        for path, text in self.sources.items():
            if path.endswith(".cpp"):
                names = {}
                for suite, name in _GTEST_DECL_RE.findall(text):
                    names[name] = suite
                    names[f"{suite}.{name}"] = suite
                    names[f"{suite}::{name}"] = suite
                self._cpp_tests[Path(path).name] = names

    # -- strategy 0: explicit, verified "covered-by:" claim ---------------
    def covered_by_claims(self, notes: str) -> List[str]:
        """Extract ``covered-by: <target>`` claims from a notes field."""
        return [m.strip() for m in _COVERED_BY_RE.findall(notes or "")]

    def verify_claim(self, claim: str) -> str:
        """Verify one ``covered-by`` target. Returns "" when the claim holds,
        else a human-readable reason why it does not.

        A claim that cannot be verified is WORSE than no claim: it is an
        assertion of coverage with nothing behind it. Never treat an
        unverifiable claim as resolution.
        """
        if "::" not in claim:
            return f"malformed covered-by claim (no '::'): {claim!r}"
        path, target = claim.split("::", 1)
        path = path.strip()
        target = target.strip()
        if "DISABLED_" in target:
            return f"claimed test is DISABLED_: {claim}"
        text = self.sources.get(path)
        if text is None:
            return f"covered-by names a file not present in tests/: {path}"
        if path.endswith(".cpp"):
            declared = self._cpp_tests.get(Path(path).name, {})
            if target in declared:
                return ""
            return (f"covered-by names {target!r} which is not declared in "
                    f"{path}")
        # pytest target: must appear as a collected node id (exact or
        # parametrised) so a renamed/deleted test is caught.
        wanted = f"{path}::{target}"
        for node in self.node_ids:
            if node == wanted or node.startswith(wanted + "["):
                return ""
        if not self.node_ids:
            return (f"covered-by pytest claim {wanted!r} could not be checked "
                    f"— no collected node ids available")
        return f"covered-by names a pytest test that was not collected: {wanted}"

    # -- strategy 1: pytest parametrisation id ---------------------------
    def param_node_for(self, test_id: str) -> Optional[str]:
        """Return the collected node id whose parametrisation contains
        *test_id* as a whole id component, else None."""
        for node in self.node_ids:
            if "[" not in node:
                continue
            params = node[node.index("[") + 1:node.rindex("]")]
            if test_id in params.split("-") or params == test_id:
                return node
        return None

    # -- strategy 2: literal occurrence in a test source -----------------
    def source_hit_for(self, test_id: str) -> Optional[str]:
        for path, text in self.sources.items():
            if test_id in text:
                return path
        return None

    # -- strategy 3: notes naming a gtest in a real .cpp ------------------
    def notes_gtest_for(self, notes: str) -> Optional[str]:
        for cpp_name in _CPP_FILE_IN_NOTES_RE.findall(notes or ""):
            declared = self._cpp_tests.get(cpp_name)
            if declared is None:
                continue  # notes name a .cpp that does not exist
            for ident in _IDENT_RE.findall(notes):
                if ident in declared:
                    return f"{cpp_name}::{ident}"
            # A dotted/scoped "Suite.Name" / "Suite::Name" spelling.
            for token in re.findall(
                    r"\b[A-Za-z_][A-Za-z0-9_]*(?:::|\.)[A-Za-z_][A-Za-z0-9_]*",
                    notes):
                if token in declared:
                    return f"{cpp_name}::{token}"
        return None

    # -- disabled detection ----------------------------------------------
    def gtest_disabled(self, evidence: str) -> str:
        """Return a reason string if the referenced gtest is DISABLED_."""
        if "DISABLED_" in evidence:
            return f"gtest is DISABLED_: {evidence}"
        return ""

    def python_blanket_skip(self, path: str, test_id: str) -> str:
        """Return a reason if the literal occurrence of *test_id* in *path*
        sits inside a test function carrying an UNCONDITIONAL
        ``@pytest.mark.skip`` (a conditional ``skipif`` is environmental and
        does not, by itself, invalidate the mapping)."""
        text = self.sources.get(path, "")
        if not path.endswith(".py") or test_id not in text:
            return ""
        idx = text.index(test_id)
        # Walk back to the enclosing "def test_..." and inspect its decorators.
        starts = [m.start() for m in _PY_DEF_RE.finditer(text) if m.start() < idx]
        if not starts:
            return ""
        head_start = text.rfind("\n\n", 0, starts[-1])
        decorators = text[max(0, head_start):starts[-1]]
        if re.search(r"@pytest\.mark\.skip\s*\(", decorators):
            return f"resolving test in {path} is unconditionally skipped"
        return ""


def resolve_row(row: Dict[str, str], index: TestIndex) -> Resolution:
    """Resolve a single matrix row against *index*."""
    test_id = (row.get("test_id") or "").strip()
    status = (row.get("status") or "").strip()
    res = Resolution(test_id=test_id, status=status)
    notes = row.get("notes", "") or ""

    claims = index.covered_by_claims(notes)
    if claims:
        reasons = [index.verify_claim(c) for c in claims]
        good = [c for c, why in zip(claims, reasons) if not why]
        res.kind = "covered-by"
        if good:
            res.evidence = "; ".join(good)
        else:
            res.evidence = "; ".join(claims)
            res.disabled_reason = "; ".join(w for w in reasons if w)
        return res

    node = index.param_node_for(test_id)
    if node is not None:
        res.kind, res.evidence = "param", node
        return res

    path = index.source_hit_for(test_id)
    if path is not None:
        res.kind, res.evidence = "source", path
        res.disabled_reason = (index.python_blanket_skip(path, test_id)
                               or index.gtest_disabled(test_id))
        return res

    ev = index.notes_gtest_for(notes)
    if ev is not None:
        res.kind, res.evidence = "notes-gtest", ev
        res.disabled_reason = index.gtest_disabled(ev)
        return res

    return res


def resolve_rows(rows: Iterable[Dict[str, str]],
                 index: TestIndex) -> List[Resolution]:
    return [resolve_row(r, index) for r in rows]


def unresolved_claiming_rows(
        resolutions: Iterable[Resolution],
        claiming: Iterable[str] = CLAIMING_STATUSES) -> List[Resolution]:
    """Rows whose status CLAIMS a pass but which resolve to no live test.

    These are the failures: the matrix asserts coverage that the repository
    cannot substantiate.
    """
    claim = set(claiming)
    return [r for r in resolutions
            if r.status in claim and not r.resolved]


# ---------------------------------------------------------------------------
# I/O wrappers — build a TestIndex from the working tree
# ---------------------------------------------------------------------------

def collect_node_ids(targets: Sequence[str] = ("tests/ui_automation",
                                               "tests/visual"),
                     repo_root: Optional[Path] = None,
                     timeout: float = 900.0) -> List[str]:
    """Run ``pytest --collect-only -q`` and return the collected node ids.

    Collection imports test modules but runs no test, launches no application
    and needs no display.
    """
    root = Path(repo_root or REPO_ROOT)
    proc = subprocess.run(
        [sys.executable, "-m", "pytest", *targets, "--collect-only", "-q",
         "-p", "no:cacheprovider"],
        cwd=str(root), capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=timeout,
    )
    return [ln.strip() for ln in proc.stdout.splitlines() if "::" in ln]


def load_test_sources(repo_root: Optional[Path] = None) -> Dict[str, str]:
    """Read every Python/C++ test source under ``tests/`` (excluding the
    matrix dir, runtime scratch and fixtures — see ``_EXCLUDED_DIR_PARTS``)."""
    root = Path(repo_root or REPO_ROOT)
    out: Dict[str, str] = {}
    for dirpath, _dirs, files in os.walk(root / "tests"):
        rel_dir = "/" + str(Path(dirpath).relative_to(root)).replace(
            os.sep, "/") + "/"
        if any(part in rel_dir for part in _EXCLUDED_DIR_PARTS):
            continue
        for fn in files:
            if not fn.endswith((".py", ".cpp")):
                continue
            p = Path(dirpath) / fn
            rel = str(p.relative_to(root)).replace(os.sep, "/")
            try:
                out[rel] = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
    return out


def build_index(repo_root: Optional[Path] = None,
                collect: bool = True) -> TestIndex:
    nodes = collect_node_ids(repo_root=repo_root) if collect else []
    return TestIndex(node_ids=nodes, sources=load_test_sources(repo_root))


def format_report(resolutions: Sequence[Resolution]) -> str:
    """Human-readable summary: how many rows resolved, by which evidence."""
    from collections import Counter
    kinds = Counter(r.kind or "UNRESOLVED" for r in resolutions)
    lines = ["Matrix row -> test resolution:"]
    for k, v in sorted(kinds.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"  {k:14s} {v:4d}")
    disabled = [r for r in resolutions if r.disabled_reason]
    if disabled:
        lines.append(f"  (of which disabled/blanket-skipped: {len(disabled)})")
    return "\n".join(lines)
