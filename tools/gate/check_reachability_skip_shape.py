#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AST statement-shape detector for the WIN-45 reachability-skip false green.

Why this exists
---------------
`phase-25-dispositions.md` section 9.5 records the standing lesson of this
programme:

    Three phases have now keyed a sweep on a phrase and been wrong. ... The
    durable form of this check keys on the **statement shape**: a guard whose
    failure branch exits without failing. That is an AST property, not a string
    property, and no phrase list can express it.

This module is that durable form. It **never reads the skip message**. Not one
rule in `_classify_branch`, `_branch_fails`, or `_condition_provenance` looks at
a string literal's content, so a fifth previously-unseen wording ("widget not
found", "dialog did not open", anything at all) is caught on exactly the same
footing as the four spellings F11/F25 already knew about.

The shape
---------
An `if` statement inside a test module is a finding when ALL of:

  (a) EXITS  -- the branch reached when the guard fails leaves the test early:
      it contains a terminal `pytest.skip(...)`, `raise SkipTest(...)`,
      `self.skipTest(...)`, `pytest.xfail(...)`, `return`, or `continue`.
  (b) DOES NOT FAIL -- that same branch contains no failing statement:
      no `pytest.fail`, no `self.fail`, no `assert`, no `raise AssertionError`
      and no other raise apart from the skip exceptions themselves.
  (c) PROBES -- the guard condition's truth value derives from a CALL, either
      written inline in the condition or reached through a local assignment
      chain. A runtime probe, not a static predicate. This is what makes it a
      *lookup* guard rather than a constant one.
  (d) IS NOT AN ENVIRONMENT GATE -- the provenance of every call feeding the
      condition is outside the environment allowlist (`os.environ`, `os.getenv`,
      `sys.platform`, `platform.*`, filesystem-existence probes on values that
      are demonstrably paths, `shutil.which`, `importlib.util.find_spec`,
      `pytest.importorskip`, ...). Environment gates are the legitimate use of
      `pytest.skip` that `tests/test_ui_annotations.py:72` documents, and
      flagging them would make this checker useless.

(a) and (b) together are the false green: the guard notices the product could
not be driven and reports the run as *not a failure*. (c) and (d) separate that
from the legitimate "this machine cannot run this test" gate.

Split record/skip shape
-----------------------
The third recurrence of WIN-45 hid the reachability wording in a
`record_matrix_result(..., "blocked", msg)` call and left the `pytest.skip`
message innocuous. Shape-wise that is just a branch with a bookkeeping call and
a skip, and no fail -- so it matches (a)+(b) with no special case at all. That
is the point of keying on shape.

Usage
-----
    python tools/gate/check_reachability_skip_shape.py --paths tests
    python tools/gate/check_reachability_skip_shape.py --paths tests --json
    python tools/gate/check_reachability_skip_shape.py --paths tests \\
        --baseline tools/gate/reachability_skip_baseline.json

With `--baseline` the checker is a RATCHET: findings already recorded in the
baseline are reported as known and do not fail the run; any finding that is not
in the baseline exits non-zero. `--update-baseline` rewrites it.

The baseline is BIDIRECTIONAL (GitHub #204)
-------------------------------------------
Until #204 the ratchet asked one question only -- "is this live finding in the
baseline?" -- and never the converse, "is this baseline entry still live?".
`check_magnitude_guard.py:483` has always asked both. A one-directional ratchet
rots: an entry whose guard was deleted or rewritten stays in the file forever,
and (when entries are line-keyed) can silently absorb a DIFFERENT guard that
later lands on that line without anyone reviewing it. So a baseline entry that
matches nothing live is a STALE entry and fails the run, with an explicit
`--allow-stale-baseline` escape for the case where you want to see the rest of
the report first.

Why entries are fingerprinted, not just line-numbered (schema 2)
----------------------------------------------------------------
`known_sites` is keyed on `file:line`. With a 400+ entry population spread over
80+ files, ANY edit above a guard shifts its line: the same, already-reviewed
skip then re-reports as NEW *and* its old entry reads as STALE, and
`--update-baseline` becomes the path of least resistance -- which re-baselines
everything else that drifted at the same time. Making the check bidirectional
without fixing that would have made it a guard that is red for innocent reasons,
i.e. decoration.

So schema 2 adds `known_fingerprints`. A fingerprint is

    <file>|<function>|<exit_kind>|<normalised condition text>#<ordinal>

-- no line number in it. The ordinal disambiguates guards that are identical in
all four fields within one file (assigned in line order), so distinct guards can
never collide onto one entry. Fingerprints are AUTHORITATIVE for both questions:
a live finding is "known" when its fingerprint is in the baseline, and a
baseline fingerprint is "stale" when no live finding carries it. `known_sites`
is retained, regenerated on every `--update-baseline`, and remains the
human-readable index of where the entries are today (`tools/gate/
test_f35_tranche_controls.py` reads it); it is NOT consulted when deciding
whether a finding is known, precisely so that a drifted line number cannot
absorb an unreviewed guard. Line drift is reported as DRIFT, informational.

A baseline with no `known_fingerprints` key (schema 1) still works: matching and
staleness both fall back to `file:line`, exactly the `check_magnitude_guard.py`
shape.

Exit codes: 0 = clean, 1 = new findings and/or stale baseline entries,
2 = usage/parse error.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set

# --------------------------------------------------------------------------
# Vocabulary. NOTE: every entry below names a CALL TARGET or an ATTRIBUTE PATH.
# None of them is a message phrase. Adding a word to any of these lists cannot
# make the checker message-sensitive.
# --------------------------------------------------------------------------

# (a) terminal exits that leave the test without failing it
SKIP_CALLS = frozenset(
    {
        "pytest.skip",
        "pytest.xfail",
        "unittest.skip",
        "skip",
        "skipTest",
        "self.skipTest",
        "self.skip",
    }
)
SKIP_EXCEPTIONS = frozenset(
    {
        "SkipTest",
        "unittest.SkipTest",
        "pytest.skip.Exception",
        "Skipped",
        "_pytest.outcomes.Skipped",
    }
)

# (b) statements that DO fail the test
FAIL_CALLS = frozenset(
    {
        "pytest.fail",
        "self.fail",
        "fail",
        "pytest.exit",
    }
)

# (d) provenances that make a guard an environment gate rather than a probe of
#     the product under test
ENV_CALLS = frozenset(
    {
        "os.getenv",
        "os.environ.get",
        "os.environ.setdefault",
        "os.path.exists",
        "os.path.isfile",
        "os.path.isdir",
        "os.path.lexists",
        "os.access",
        "shutil.which",
        "platform.system",
        "platform.machine",
        "platform.release",
        "importlib.util.find_spec",
        "importlib.find_loader",
        "pytest.importorskip",
        "sys.getwindowsversion",
        "sysconfig.get_platform",
    }
)
# Attribute paths whose mere presence in a condition marks it environmental.
ENV_ATTRS = frozenset(
    {
        "sys.platform",
        "os.name",
        "sys.version_info",
        "os.environ",
        "sys.maxsize",
    }
)
# GitHub #305, fifth correction. Module roots whose ATTRIBUTE reads cannot be a
# probe of the product, used by the attribute arm of `_chain_is_value_in_hand`.
# Deliberately only stdlib namespaces that hold constants and pure functions --
# no object a UIA wrapper could be reached through. Membership is by the root
# identifier only, so shadowing it with a local (`os = session.os`) would be
# unsound; nothing in this repo does, and an entry here must stay a module.
SAFE_MODULE_ROOTS = frozenset(
    {"os", "sys", "platform", "string", "re", "math", "json", "shutil"}
)
# Path-object methods. Only treated as environmental when the receiver is
# provably a filesystem path (see _pathlike_names).
PATH_METHODS = frozenset({"exists", "is_file", "is_dir", "resolve", "stat"})
# Calls that PRODUCE a filesystem path, used to type local receivers.
PATH_FACTORIES = frozenset(
    {
        "Path",
        "pathlib.Path",
        "os.path.join",
        "os.fspath",
        "os.path.abspath",
        "os.path.dirname",
        "os.path.realpath",
        "tempfile.mkdtemp",
    }
)
# Path-preserving methods: applying one to a path yields a path.
PATH_DERIVERS = frozenset(
    {
        "resolve",
        "absolute",
        "expanduser",
        "joinpath",
        "with_suffix",
        "with_name",
    }
)


@dataclass(frozen=True)
class Finding:
    """One structurally-matching guard."""

    file: str
    line: int
    function: str
    exit_kind: str  # "skip" | "return" | "continue" | "raise-skip" | "xfail"
    condition: str  # source text of the guard condition, for the human reader

    def key(self) -> str:
        return "%s:%d" % (self.file, self.line)

    def shape(self) -> str:
        """Line-independent identity of this guard.

        Everything that makes the guard what it is, and nothing about WHERE in
        the file it sits. `function` and `exit_kind` cannot contain the `|`
        separator; `condition` is the last field, so the rendering is
        unambiguous even when the condition text contains one.
        """
        return "%s|%s|%s|%s" % (self.file, self.function, self.exit_kind, self.condition)


# --------------------------------------------------------------------------
# dotted-name helpers
# --------------------------------------------------------------------------


def _dotted(node: ast.AST) -> Optional[str]:
    """Render Name/Attribute chains as a dotted string; None for anything else."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        base = _dotted(node.value)
        return None if base is None else base + "." + node.attr
    return None


def _call_name(node: ast.Call) -> Optional[str]:
    return _dotted(node.func)


def _tail(name: Optional[str]) -> Optional[str]:
    return None if name is None else name.rsplit(".", 1)[-1]


# --------------------------------------------------------------------------
# (a)/(b): branch classification
# --------------------------------------------------------------------------


def _walk_shallow(stmts: Sequence[ast.stmt]) -> Iterable[ast.AST]:
    """Every node in `stmts`, but NOT descending into nested function/class defs.

    A nested helper defined inside the branch has its own control flow; its
    `return` is not the test's exit and its `assert` is not the test's failure.
    """
    for stmt in stmts:
        if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            continue
        yield stmt
        for child in ast.iter_child_nodes(stmt):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                continue
            yield from _walk_shallow([child]) if isinstance(child, ast.stmt) else _walk_expr(child)


def _walk_expr(node: ast.AST) -> Iterable[ast.AST]:
    yield node
    for child in ast.iter_child_nodes(node):
        if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            continue
        yield from _walk_expr(child)


def _branch_exit_kind(stmts: Sequence[ast.stmt], in_test: bool) -> Optional[str]:
    """Return the kind of early exit this branch performs, or None.

    `return` and `continue` only count inside a function that IS a test. In a
    helper they are ordinary control flow -- `if args.list: return` in a CLI
    `main()` is not a false green, and counting it produced 2474 findings on
    this tree, almost all of them noise. A `pytest.skip` / `raise SkipTest`
    aborts the calling test wherever it is written, so those count everywhere.
    """
    kind: Optional[str] = None
    for node in _walk_shallow(stmts):
        if isinstance(node, ast.Call):
            name = _call_name(node)
            if name in SKIP_CALLS or _tail(name) in {"skipTest"}:
                return "xfail" if name == "pytest.xfail" else "skip"
        elif isinstance(node, ast.Raise) and node.exc is not None:
            exc = node.exc
            target = exc.func if isinstance(exc, ast.Call) else exc
            name = _dotted(target)
            if name in SKIP_EXCEPTIONS or _tail(name) in {"SkipTest", "Skipped"}:
                return "raise-skip"
        elif in_test and isinstance(node, ast.Return) and kind is None:
            kind = "return"
        elif in_test and isinstance(node, ast.Continue) and kind is None:
            kind = "continue"
    return kind


def _deferred_failure_names(func: ast.AST) -> Set[str]:
    """Local collections whose contents are later asserted on.

    The `mismatches.append(...)` / `continue` shape inside a validation loop is
    a DEFERRED failure, not a false green: the loop keeps going and the function
    fails at the end on the accumulated list. Structurally recognisable -- the
    name appended to also appears inside an assert or a `pytest.fail` argument
    in the same function -- so it is recognised here rather than left to a
    message heuristic.
    """
    asserted: Set[str] = set()
    for node in ast.walk(func):
        target: Optional[ast.AST] = None
        if isinstance(node, ast.Assert):
            target = node
        elif isinstance(node, ast.Call) and _call_name(node) in FAIL_CALLS:
            target = node
        if target is None:
            continue
        for sub in ast.walk(target):
            if isinstance(sub, ast.Name):
                asserted.add(sub.id)
    return asserted


def _branch_defers_failure(stmts: Sequence[ast.stmt], asserted: Set[str]) -> bool:
    for node in _walk_shallow(stmts):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            if node.func.attr in {"append", "add", "extend", "update"}:
                root = _dotted(node.func.value)
                if root and root.split(".", 1)[0] in asserted:
                    return True
        if isinstance(node, ast.AugAssign):
            root = _dotted(node.target)
            if root and root.split(".", 1)[0] in asserted:
                return True
    return False


def _branch_fails(stmts: Sequence[ast.stmt]) -> bool:
    """True when the branch actually fails the test (so it is NOT a false green)."""
    for node in _walk_shallow(stmts):
        if isinstance(node, ast.Assert):
            return True
        if isinstance(node, ast.Call):
            name = _call_name(node)
            if name in FAIL_CALLS:
                return True
            tail = _tail(name)
            # unittest-style assertions: assertTrue / assertEqual / assertIsNotNone ...
            if tail and tail.startswith("assert") and tail != "assert_":
                return True
            # gtest-ish / helper-ish explicit failure raisers
            if tail in {"fail", "failNow", "raise_for_status"}:
                return True
        if isinstance(node, ast.Raise):
            exc = node.exc
            if exc is None:  # bare re-raise
                return True
            target = exc.func if isinstance(exc, ast.Call) else exc
            name = _dotted(target)
            if name in SKIP_EXCEPTIONS or _tail(name) in {"SkipTest", "Skipped"}:
                continue
            return True
    return False


# --------------------------------------------------------------------------
# (c)/(d): condition provenance
# --------------------------------------------------------------------------


def _target_binds(target: ast.AST, name: str) -> bool:
    """True when assigning to `target` binds `name` (incl. tuple unpacking)."""
    return any(
        isinstance(n, ast.Name) and n.id == name for n in ast.walk(target)
    )


class _AssignmentIndex:
    """Most recent assignment to each local name, by source line.

    Deliberately flow-insensitive and cheap: for the guard shapes this checker
    targets (`w = find(...)` a line or two above `if w is None:`) a linear
    "latest assignment at or before this line" lookup is exact.

    **Where it is NOT exact, the direction depends on which accessor you use,
    and that mattered.** This docstring used to claim flatly that it "errs
    toward reporting rather than hiding". For `latest_before` that is false: a
    conditional rebinding whose innocuous branch is written second HIDES the
    lookup in the other branch, which the p98 phase-3 claim-check gate measured
    as a zero-finding false green. `latest_before` keeps its single-answer
    semantics for the provenance walk that wants a most-likely source; anything
    deciding an EXEMPTION must use `all_before`, which returns every candidate
    so the caller can require all of them to be harmless.
    """

    def __init__(self, func: ast.AST, recurse_into_functions: bool = True) -> None:
        self._by_name: Dict[str, List[tuple]] = {}
        self._root = func
        # id(stmt) -> (containing block list, index in it, owning node).
        # `reaching()` needs structure, not line numbers; see its docstring for
        # why neither line-ordered accessor above can be made sound.
        self._stmt_pos: Dict[int, tuple] = {}
        self._parent: Dict[int, ast.AST] = {}
        for parent in ast.walk(func):
            for _field, value in ast.iter_fields(parent):
                children = value if isinstance(value, list) else [value]
                for child in children:
                    if isinstance(child, ast.AST):
                        self._parent[id(child)] = parent
                if not isinstance(value, list):
                    continue
                if value and all(isinstance(v, ast.stmt) for v in value):
                    for i, st in enumerate(value):
                        self._stmt_pos[id(st)] = (value, i, parent)
        if recurse_into_functions:
            nodes: Iterable[ast.AST] = ast.walk(func)
        else:
            nodes = [
                n
                for n in ast.iter_child_nodes(func)
                if not isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
            ]
        for node in nodes:
            targets: List[ast.expr] = []
            value: Optional[ast.expr] = None
            if isinstance(node, ast.Assign):
                targets, value = list(node.targets), node.value
            elif isinstance(node, (ast.AnnAssign, ast.AugAssign)):
                targets, value = [node.target], node.value
            elif isinstance(node, ast.NamedExpr):
                targets, value = [node.target], node.value
            elif isinstance(node, ast.withitem):  # `with open(x) as f:`
                if node.optional_vars is not None:
                    targets, value = [node.optional_vars], node.context_expr
            if value is None:
                continue
            for target in targets:
                for name_node in ast.walk(target):
                    if isinstance(name_node, ast.Name):
                        self._by_name.setdefault(name_node.id, []).append(
                            (getattr(node, "lineno", 0), value)
                        )
        for entries in self._by_name.values():
            entries.sort(key=lambda item: item[0])

    def latest_before(self, name: str, line: int) -> Optional[ast.expr]:
        best = None
        for lineno, value in self._by_name.get(name, []):
            if lineno <= line:
                best = value
            else:
                break
        return best

    def all_before(self, name: str, line: Optional[int]) -> List[ast.expr]:
        """EVERY binding of `name` at or before `line`, not just the last one.

        `line=None` means EVERY binding in the function, with no line cutoff.
        That is what the exemption uses, and it is not belt-and-braces: a guard
        inside a loop is reached again after a binding written BELOW it --

            t = ""
            for w in session.titles():
                if t.startswith("P"):
                    continue                        # line 3
                t = session.find_window().title()   # line 5, reaches line 3
                                                    #   on the next iteration

        -- so a cutoff at the guard's own line sees only the empty string and
        exempts the site. Found by self-probing the shapes the gate had not got
        to yet, after it had already caught the conditional-rebinding case.
        Dropping the cutoff moved the live tree not at all (413/413), so the
        strictness costs nothing this repo writes.

        GitHub #305, SEVENTH correction. `latest_before` returns the
        TEXTUALLY last binding, which is not the same as the one that ran. The
        p98 phase-3 claim-check gate drove `scan_source` with the ordinary way
        anyone writes "use the real value if special-cased, else a default":

            if cond:
                t = session.find_window().title()
            else:
                t = "default"
            if t.startswith("P"):
                continue

        Zero findings -- the `else` branch's literal sits on the later line, so
        the binding chase was handed the literal and never saw the lookup.
        Swapping the two branch bodies flags it correctly: same semantics,
        opposite verdict, decided by which branch was written second.

        MULTIPLICITY rather than path-sensitivity: a value is in hand only if
        EVERY binding that could have produced it is in hand. One lookup among
        the candidates is a lookup, whichever branch wrote it.

        **Retained only as the fallback for a caller with no guard statement to
        anchor on.** `reaching()` supersedes it for the exemption decision --
        see there for why both line-ordered answers turned out unsound, one in
        each direction.
        """
        if line is None:
            return [value for _lineno, value in self._by_name.get(name, [])]
        return [value for lineno, value in self._by_name.get(name, []) if lineno <= line]

    # ------------------------------------------------------------------
    # reaching definitions (GitHub #305, EIGHTH correction)
    # ------------------------------------------------------------------

    def reaching(self, name: str, guard: ast.stmt) -> List[Optional[ast.expr]]:
        """Bindings of `name` that can be the one in effect AT `guard`.

        GitHub #305, eighth correction. Two line-ordered approximations were
        tried here and the p98 phase-3 claim-check gate falsified both, one in
        each direction:

        * `latest_before` -- the TEXTUALLY last binding. Unsound: a conditional
          rebinding whose innocuous branch is written second hid the lookup in
          the other branch. Zero findings on a real probe.
        * every binding in the function, no cutoff -- sound but OVER-STRICT. A
          name legitimately reused for two unrelated purposes in one long test
          function reddened a guard that has no probe anywhere in its reach.
          In a push-blocking gate that is not a cosmetic cost: #305 is explicit
          that neither baselining the false positive nor rewriting correct code
          around a checker bug is an acceptable answer.

        A cutoff cannot separate those, because the two cases differ in
        STRUCTURE and not in line order: the loop-carried binding is below the
        guard and reaches it, while the reused name's earlier binding is above
        the guard and does not. So this computes the reaching set properly --
        backwards through preceding siblings, outwards through enclosing blocks,
        stopping at the first binding that definitely kills, and taking the
        back-edge of every loop that encloses the guard.

        An element of the result is `None` for a binding whose value cannot be
        named -- a `for` target, where the AST offers the iterable and not the
        element. That is the disclosed residual, and `None` is PERMITTED by the
        caller, which is what makes the residual a residual rather than a hole
        this function pretends to have closed.

        An empty result means no binding was found at all (a parameter, an
        import, a module-level constant) and the caller permits that too.
        """
        out: List[Optional[ast.expr]] = []
        cur: Optional[ast.AST] = guard
        while cur is not None:
            pos = self._stmt_pos.get(id(cur))
            if pos is None:
                break
            block, idx, owner = pos
            # Every loop that encloses the guard has a back edge: a binding
            # BELOW the guard in that loop's body reaches it on the next
            # iteration. Collect those before deciding about the kill, because
            # a kill above does not stop a rebinding below.
            if isinstance(owner, (ast.For, ast.AsyncFor, ast.While)):
                for later in block[idx + 1:]:
                    self._bindings_in_stmt(later, name, out)
                if isinstance(owner, (ast.For, ast.AsyncFor)) and _target_binds(
                        owner.target, name):
                    out.append(None)  # the element itself -- unnameable
            if self._scan_back(block, idx, name, out):
                break
            # The statement OWNING this block can bind the name itself, and
            # scanning that block's siblings never sees it: a guard inside
            # `with session.open_window() as t:` has no preceding sibling, so
            # stepping straight out missed the binding entirely (measured).
            if isinstance(owner, (ast.With, ast.AsyncWith)):
                bound = False
                for item in owner.items:
                    if item.optional_vars is not None and _target_binds(
                            item.optional_vars, name):
                        out.append(item.context_expr)
                        bound = True
                if bound:
                    break
            if isinstance(owner, ast.ExceptHandler) and owner.name == name:
                out.append(None)  # the exception object -- unnameable
                break
            if isinstance(owner, (ast.FunctionDef, ast.AsyncFunctionDef,
                                  ast.ClassDef, ast.Module)):
                break
            # Step out: continue the walk from the position of the statement
            # that owns this block. An ExceptHandler is not a statement, so go
            # up one more to its `try`.
            cur = owner if isinstance(owner, ast.stmt) else self._parent.get(id(owner))
        return out

    def _scan_back(
        self, block: List[ast.stmt], idx: int, name: str, out: List[Optional[ast.expr]]
    ) -> bool:
        """Walk preceding siblings; True once a binding definitely kills."""
        for i in range(idx - 1, -1, -1):
            if self._bindings_in_stmt(block[i], name, out):
                return True
        return False

    def _scan_branch(
        self, block: List[ast.stmt], name: str, out: List[Optional[ast.expr]]
    ) -> bool:
        return self._scan_back(block, len(block), name, out)

    def _bindings_in_stmt(
        self, st: ast.stmt, name: str, out: List[Optional[ast.expr]]
    ) -> bool:
        """Append `name`'s bindings in `st`; True if `st` DEFINITELY rebinds it.

        "Definitely" is the only thing allowed to stop the backwards walk, so
        every uncertain case returns False and the walk keeps looking -- which
        adds candidates and can only make the caller stricter.
        """
        if isinstance(st, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            return False  # a different scope
        if isinstance(st, ast.Assign):
            if any(_target_binds(t, name) for t in st.targets):
                out.append(st.value)
                return True
            return self._named_exprs(st, name, out)
        if isinstance(st, ast.AnnAssign):
            if st.value is not None and _target_binds(st.target, name):
                out.append(st.value)
                return True
            return self._named_exprs(st, name, out)
        if isinstance(st, ast.AugAssign):
            if _target_binds(st.target, name):
                # Reads the previous value as well, so it does not kill.
                out.append(st.value)
            return self._named_exprs(st, name, out)
        if isinstance(st, ast.If):
            killed_body = self._scan_branch(st.body, name, out)
            killed_else = self._scan_branch(st.orelse, name, out) if st.orelse else False
            self._named_exprs(st.test, name, out)
            return bool(killed_body and killed_else)
        if isinstance(st, ast.Try):
            self._scan_branch(st.body, name, out)
            for handler in st.handlers:
                self._scan_branch(handler.body, name, out)
            if st.orelse:
                self._scan_branch(st.orelse, name, out)
            killed_final = (
                self._scan_branch(st.finalbody, name, out) if st.finalbody else False
            )
            return bool(killed_final)
        if isinstance(st, (ast.For, ast.AsyncFor)):
            if _target_binds(st.target, name):
                out.append(None)
            self._scan_branch(st.body, name, out)
            if st.orelse:
                self._scan_branch(st.orelse, name, out)
            self._named_exprs(st.iter, name, out)
            return False  # the loop body may not run
        if isinstance(st, ast.While):
            self._scan_branch(st.body, name, out)
            if st.orelse:
                self._scan_branch(st.orelse, name, out)
            self._named_exprs(st.test, name, out)
            return False
        if isinstance(st, (ast.With, ast.AsyncWith)):
            killed = False
            for item in st.items:
                if item.optional_vars is not None and _target_binds(
                        item.optional_vars, name):
                    out.append(item.context_expr)
                    killed = True
            self._scan_branch(st.body, name, out)
            return killed
        return self._named_exprs(st, name, out)

    def _named_exprs(
        self, node: ast.AST, name: str, out: List[Optional[ast.expr]]
    ) -> bool:
        """Collect `:=` bindings of `name` anywhere inside `node`.

        Never reported as a kill: a walrus inside a `BoolOp` or a comprehension
        may not evaluate, and claiming otherwise would stop the walk early --
        the one thing that can make this unsound.
        """
        for sub in ast.walk(node):
            if isinstance(sub, ast.NamedExpr) and _target_binds(sub.target, name):
                out.append(sub.value)
        return False

    def pathlike_names(self, seed: Optional[Set[str]] = None) -> Set[str]:
        """Names whose assigned value is a filesystem path.

        Two producers count: a path FACTORY call (`Path(...)`, `os.path.join`),
        and the pathlib `/` operator applied to something already known to be a
        path (`FIXTURES / "x.pdf"`, `REPO_ROOT / "tests" / name`). The second is
        not optional -- `fixture = FIXTURES / "test_single.pdf"` followed by
        `if not fixture.exists(): pytest.skip(...)` is the single most common
        legitimate fixture gate in this tree, and without the operator rule it
        reads as a product probe.
        """
        out: Set[str] = set(seed or ())

        def produces_path(value: ast.expr) -> bool:
            if isinstance(value, ast.Call):
                if _call_name(value) in PATH_FACTORIES:
                    return True
                # Path-preserving methods: Path(__file__).resolve(), p.joinpath(x)
                if isinstance(value.func, ast.Attribute) and value.func.attr in PATH_DERIVERS:
                    return produces_path(value.func.value)
                return False
            if isinstance(value, ast.BinOp) and isinstance(value.op, ast.Div):
                return produces_path(value.left)
            if isinstance(value, ast.Attribute) and value.attr in {"parent", "parents"}:
                return produces_path(value.value)
            if isinstance(value, ast.Subscript):
                return produces_path(value.value)
            root = _dotted(value)
            return bool(root) and root.split(".", 1)[0] in out

        # Iterate to a fixpoint: `_HERE = Path(f).resolve().parent`, then
        # `ROOT = _HERE.parent`, then `FIXTURES = ROOT / "tests" / "fixtures"`
        # is a three-link chain and dict order is not assignment order.
        for _ in range(6):
            before = len(out)
            for name, entries in self._by_name.items():
                if name in out:
                    continue
                for _lineno, value in entries:
                    if produces_path(value):
                        out.add(name)
                        break
            if len(out) == before:
                break
        return out


@dataclass
class _Provenance:
    has_call: bool = False
    all_calls_environmental: bool = True
    saw_env_attr: bool = False


#: Builtin predicates that inspect a VALUE ALREADY IN HAND. They cannot be
#: reachability probes: they never go and look for anything, so their failure
#: branch cannot mean "the product could not be driven".
#:
#: Added because `for key in (...): if not isinstance(rec.get(key), str):
#: continue` -- an ordinary type filter skipping a dict KEY, not a test -- was
#: reported as a WIN-45 false green. A guard that reddens on correct code gets
#: muted, and then it is not there for the real case; this checker is worth too
#: much for that.
#: Deliberately NOT here: `len` and `hasattr`. `if not len(find_all(...))` and
#: `if not hasattr(session, "canvas")` are genuine product probes, and widening
#: to them would blind the checker to the exact class it exists for. Only
#: predicates that answer a question about a value's TYPE qualify.
#:
#: KNOWN BLIND SPOT, measured rather than assumed: a type predicate wrapped
#: directly around a live lookup -- `if not isinstance(session.find_optional(
#: ...), X): pytest.skip(...)` -- is now quiet, and it is a real false green.
#: Accepted because nobody writes a lookup guard that way (the idiom is
#: `is None`), and the alternative was 365-file noise elsewhere plus a standing
#: false positive here. If that shape ever appears, this is why it was missed.
VALUE_PREDICATES = frozenset({
    "isinstance", "issubclass", "callable",
})

#: GitHub #305. STRING predicates on a value already in hand — the same
#: reasoning as VALUE_PREDICATES, reached through a METHOD rather than a
#: builtin, which is why name-based classification missed them.
#:
#: The case that prompted this, and the reason #305 stayed open after its other
#: three sites were converted:
#:
#:     for line in src.splitlines():
#:         s = line.strip()
#:         if s.startswith("#"):
#:             continue
#:
#: That is skipping COMMENT LINES while parsing source text. Nothing is being
#: driven and nothing can be unreachable, yet it matched the WIN-45 shape
#: (`continue` under a guard) and made this checker red on `master`. #305 was
#: explicit that the fix belonged here — *"the fourth needs the CHECKER taught,
#: not the baseline grown"* — and that adding the site to the baseline would
#: grow a burn-down list with an entry that has nothing to burn down.
#:
#: Why this cannot hide a real reachability probe: every name here answers a
#: question about the CONTENT of a string the caller already holds. A product
#: probe has to go and look — `find_by_suffix`, `wait_for_window`,
#: `top_window`, a `is None` on a lookup result. None of those is a string
#: method, and text obtained FROM the product (a log line, a pipe read) is
#: still text in hand by the time it is being parsed.
#:
#: Deliberately NOT here, by the same discipline the note above sets out:
#: `find`, `index`, `count`, `match`, `search`. `if line.find(x) < 0` is
#: ambiguous, and `re.search` over a product-produced buffer is a genuine
#: observation of the product's behaviour — widening to those would blind the
#: checker to a class it exists for.
TEXT_PREDICATES = frozenset({
    "startswith", "endswith",
    "isspace", "isdigit", "isalpha", "isalnum", "isidentifier",
    "islower", "isupper", "istitle", "isdecimal", "isnumeric",
})

#: String TRANSFORMS, as opposed to the predicates above. Value in, value out,
#: same value in hand -- `strip()` cannot go and look for anything.
#:
#: Needed because restricting the exemption to predicates alone over-corrected.
#: The p98 phase-3 claim-check gate probed the narrowed classifier with the
#: SINGLE-EXPRESSION form of the very idiom the exemption exists for:
#:
#:     if line.strip().startswith("#"):
#:         continue
#:
#: and it was FLAGGED, because `.strip()` is a call in the receiver chain and
#: the chain walk only tolerated predicates. The split form
#: (`s = line.strip()` then `if s.startswith("#")`) stayed exempt. Two
#: semantically identical spellings, opposite verdicts, and the chained one is
#: the more natural way to write it -- so the checker would have reddened on
#: correct code and the only ways out would have been baselining a false
#: positive (which GitHub #305 explicitly calls the wrong fix) or rewriting
#: correct code around a checker bug.
#:
#: A transform in the chain does NOT make a lookup safe:
#: `find_by_suffix(name).strip().startswith(x)` still breaks the chain on
#: `find_by_suffix`, which is not in any of these sets. That is asserted by a
#: negative control rather than left to reasoning.
#:
#: Kept deliberately tight: only methods that take a string and return a
#: string. `format` and `join` are excluded -- they interpolate OTHER values,
#: so a chain through them is no longer a statement about one value in hand.
#: Value-preserving CONVERSIONS of something already in hand. `str(x)` cannot
#: go and look anything up; it renders what it was given. Flagging
#: `str(exc).startswith("P")` is a false positive that predates all of this
#: (measured against HEAD, which flags it too), and in a push gate a false red
#: on correct code is the cost #305 says must not be paid.
#:
#: Safe because the Call arm requires every ARGUMENT to be value-in-hand first,
#: so `str(session.find_window().title()).startswith(...)` stays flagged --
#: asserted in both directions rather than argued. `format` is still excluded:
#: it interpolates other values, so a chain through it is no longer a statement
#: about one value in hand.
TEXT_CONVERSIONS = frozenset({"str", "repr", "ascii"})

TEXT_TRANSFORMS = frozenset({
    "strip", "lstrip", "rstrip",
    "lower", "upper", "casefold", "title", "swapcase",
    "removeprefix", "removesuffix", "expandtabs", "replace",
})


def _chain_is_value_in_hand(
    receiver: ast.AST,
    pathlike: Set[str],
    index: Optional["_AssignmentIndex"] = None,
    line: int = 0,
    seen: Optional[Set[str]] = None,
    guard: Optional[ast.stmt] = None,
) -> bool:
    """True when an expression is built ONLY from permitted value-in-hand shapes.

    GitHub #305. This is a WHITELIST, and the fourth version of this function.
    The three before it were blacklists -- "walk the tree and reject the bad
    things" -- and each one shipped a hole, because a blacklist has to learn
    about every construct that can carry a lookup.

    The one that ended the approach, found by the p98 phase-3 claim-check gate
    driving the live classifier:

        if windows[0].title().startswith("P"):
            continue

    was exempted. The previous version used `ast.walk()` and inspected only
    `ast.Call` nodes, so the `Subscript` `windows[0]` was **invisible** -- and
    the single call present, `.title()`, collides by name with the string
    method `str.title()` while here being a UI accessor. Neither half alone
    fails: `find_by_suffix(n).title()...` is flagged, and
    `windows[0].window_text()...` is flagged. Only the combination slipped.

    Patching `Subscript` would have been a fourth enumeration fix, with
    `BoolOp`, `IfExp`, `Compare`, comprehensions and f-strings all still
    waiting. So the logic is inverted: an expression is value-in-hand only if
    EVERY node in it is one of a small set of shapes that provably cannot look
    anything up. Anything unrecognised -- any node type not named here -- is
    not permitted, which means a construct nobody has thought of yet fails
    CLOSED into "flag it" rather than open into "exempt it".

    Permitted:
      * a literal, or a plain name;
      * an attribute chain whose ROOT is provably not a product handle -- a
        named environment attribute, a safe module, a proven filesystem path,
        or a literal (see the fifth correction in that arm; an arbitrary
        `obj.attr` is NOT permitted, because attribute access off a UIA wrapper
        is itself a lookup);
      * a call whose method is a known string transform or predicate, and whose
        receiver and every argument are themselves permitted;
      * a call `_classify_call` already recognises as environmental
        (`os.getenv(...)`), with permitted arguments.

    Everything else -- subscripts, arithmetic, boolean operators, conditional
    expressions, comprehensions, f-strings, lambdas, awaits, starred args --
    breaks the chain. Being stricter cannot manufacture a false green; it can
    only decline to exempt. The live-tree count is unchanged at 412, so it
    declines nothing this repo actually writes.
    """
    node = receiver

    if isinstance(node, ast.Constant):
        return True

    if isinstance(node, ast.Name):
        # GitHub #305, SIXTH correction. A bare name used to be value-in-hand
        # UNCONDITIONALLY, which let a single line break defeat the fifth
        # correction entirely. The p98 phase-3 claim-check gate drove
        # `scan_source` and measured:
        #
        #     if session.current_window.title.startswith("Foo"): skip   -> 1 finding
        #     title = session.current_window.title
        #     if title.startswith("Foo"): skip                          -> MISSED
        #     title = session.current_window().window_text()
        #     if title.startswith("Foo"): skip                          -> MISSED
        #
        # Same lookup, same predicate, same skip; the only difference is whether
        # the lookup sits on the line above. The old comment claimed `s` was
        # settled "regardless of where `s` came from, because asking what a
        # string BEGINS WITH cannot be a probe". The predicate is not the probe
        # -- the BINDING is, and the condition's truth depends on it.
        #
        # So chase the binding, with the SAME whitelist applied to whatever it
        # was bound from. `s = line.strip()` still resolves to a transform on a
        # name and stays exempt, which is the case the second correction had to
        # rescue; `title = session.window.title` now resolves to an attribute
        # chain off an unproven root and is flagged.
        #
        # A name already on the chain means a self-referential binding
        # (`s = s.strip()`); stop and allow, because every step traversed to get
        # back here was itself permitted -- an impermissible step would already
        # have returned False.
        if index is None:
            return True
        chased = set(seen or ())
        if node.id in chased:
            return True
        # `reaching()` when there is a guard statement to anchor on -- the
        # structural answer. `all_before(..., None)` only as a fallback, which
        # is over-strict but never unsound. See `_AssignmentIndex.reaching`.
        if guard is not None:
            sources = index.reaching(node.id, guard)
        else:
            sources = index.all_before(node.id, None)
        if not sources:
            # A parameter, a loop target, an import -- no binding expression to
            # judge. Permitted, which is the exemption's own premise. The
            # RESIDUAL this leaves is bounded and deliberate: a bare-name text
            # predicate on the element of a live iterable, whether bound by a
            # `for` target or a comprehension
            # (`for t in session.titles(): if t.startswith("P"): continue`).
            # Closing it needs the ELEMENT's type, and the AST only offers the
            # iterable -- and `for line in open(f).read().splitlines()` is
            # indistinguishable from it by that route, so chasing iteration
            # targets would redden the exact comment-filter idiom #305 says must
            # not be baselined. Anything beyond a bare name on the element
            # (`w.title`, `w.text()`) is still caught by the arms below.
            return True
        chased.add(node.id)
        # EVERY reaching binding, not the textually last one -- see
        # `_AssignmentIndex.all_before` for the conditional-rebinding case this
        # closes. One lookup among the candidates is a lookup, whichever branch
        # wrote it.
        return all(
            # `None` is a binding whose value cannot be named -- a `for`
            # target. Permitted: that is the disclosed residual, stated above.
            source is None
            or _chain_is_value_in_hand(source, pathlike, index, line, chased, guard)
            for source in sources
        )

    if isinstance(node, ast.Attribute):
        # GitHub #305, FIFTH correction -- and the first one caused by the
        # whitelist rewrite itself rather than inherited from a blacklist.
        #
        # This arm used to recurse unconditionally: `obj.attr` was value-in-hand
        # whenever `obj` was, with a comment arguing that a pure attribute chain
        # off a live object is harmless because "with no call anywhere, the
        # condition fails requirement (c) PROBES and is never a finding
        # regardless". The p98 phase-3 claim-check gate instrumented
        # `_condition_provenance` and showed that reasoning is simply FALSE as
        # soon as the chain is wrapped in a text predicate -- which is the only
        # shape this helper is ever consulted about:
        #
        #     session.current_window().window_text().startswith("Foo")  -> flagged
        #     session.current_window.title.startswith("Foo")            -> MISSED
        #     session.current_window.title.strip().startswith("Foo")    -> MISSED
        #
        # The predicate call supplies `has_call=True` itself, so (c) is
        # satisfied; permitting the receiver then set `all_calls_environmental`
        # and the lookup was laundered exactly as the three fixed `Call` holes
        # were. Attribute access is not inert in this repo either: a pywinauto
        # wrapper resolves children and round-trips to UIA on attribute READ,
        # so `.title` can be a product probe with no call syntax at all.
        #
        # So an attribute chain is value-in-hand only where its root is
        # provably not a product handle: a named environment attribute
        # (`sys.platform.startswith("win")`), a safe module root, a local the
        # path typing already proved is a filesystem path, or an attribute of a
        # literal. Any other root -- `session.`, `self.`, a fixture object --
        # breaks the chain. An opaque object's attribute cannot be told apart
        # from a live handle's by syntax, so the direction that fails closed is
        # to flag it; see the positive control in the test suite.
        dotted = _dotted(node)
        if dotted:
            if dotted in ENV_ATTRS:
                return True
            root = dotted.split(".", 1)[0]
            if root in SAFE_MODULE_ROOTS or root in pathlike:
                return True
            return False
        # Not a plain dotted chain (the receiver is an expression). Permitted
        # only when that expression is itself a literal -- `"a,b".partition`.
        return isinstance(node.value, ast.Constant)

    if isinstance(node, ast.Call):
        args_ok = all(
            _chain_is_value_in_hand(a, pathlike, index, line, seen, guard)
            for a in list(node.args) + [kw.value for kw in node.keywords]
        )
        if not args_ok:
            return False
        if isinstance(node.func, ast.Name) and node.func.id in TEXT_CONVERSIONS:
            # Arguments were already required to be value-in-hand above.
            return True
        if isinstance(node.func, ast.Attribute) and node.func.attr in (
                TEXT_TRANSFORMS | TEXT_PREDICATES):
            return _chain_is_value_in_hand(
                node.func.value, pathlike, index, line, seen, guard)
        # An environmental probe is value-in-hand for this purpose; anything
        # else is a lookup as far as we are concerned.
        return _classify_call(node, pathlike, index, line, seen, guard)

    return False


def _text_predicate_is_value_in_hand(
    call: ast.Call,
    pathlike: Set[str],
    index: Optional["_AssignmentIndex"] = None,
    line: int = 0,
    seen: Optional[Set[str]] = None,
    guard: Optional[ast.stmt] = None,
) -> bool:
    """True when a text-predicate call is wholly about values already in hand.

    GitHub #305, third correction. The receiver chain is not sufficient on its
    own, because Python lets the same predicate be written with the value as an
    ARGUMENT:

        s.startswith("P")                      # bound   -- receiver is the value
        str.startswith(s, "P")                 # unbound -- receiver is the TYPE

    In the unbound form `call.func.value` is the bare name `str`, which contains
    no calls at all, so a receiver-only check trivially passes and never looks
    at the argument -- where a live lookup can sit. Proven against the live
    classifier by the p98 phase-3 claim-check gate:

        if session.find_by_suffix("x").text().startswith("P"): ...    -> flagged
        if str.startswith(session.find_by_suffix("x").text(), "P"): ..-> MISSED

    Same guard, two spellings, opposite verdicts -- and the missed one is the
    WIN-45 false green this whole checker exists to catch.

    So the rule is the whole expression, not just the receiver: every part of a
    text-predicate call -- receiver, positional args, keyword args -- must be
    value-in-hand. That closes the unbound form and also the narrower case
    `s.startswith(find_x())`, where the predicate's truth genuinely does depend
    on a lookup even though the receiver is innocent.

    Being stricter here cannot create a false green; it can only decline to
    exempt. The live-tree count is unchanged at 412, so it declines nothing
    that is actually written in this repo.
    """
    parts = [call.func.value if isinstance(call.func, ast.Attribute) else call.func]
    parts.extend(call.args)
    parts.extend(kw.value for kw in call.keywords)
    return all(
        _chain_is_value_in_hand(part, pathlike, index, line, seen, guard)
        for part in parts
    )


def _classify_call(
    call: ast.Call,
    pathlike: Set[str],
    index: Optional["_AssignmentIndex"] = None,
    line: int = 0,
    seen: Optional[Set[str]] = None,
    guard: Optional[ast.stmt] = None,
) -> bool:
    """True when this call is an ENVIRONMENT probe rather than a product probe."""
    name = _call_name(call)
    if name in ENV_CALLS:
        return True
    if name in VALUE_PREDICATES:
        return True
    # GitHub #305. A string predicate reached as a METHOD on a value in hand --
    # but ONLY if the value really is in hand.
    #
    # The p98 phase-3 claim-check gate probed the live classifier and found the
    # hole: `find_by_suffix(x).window_text().startswith(y)` was being exempted,
    # because the outermost call is `.startswith`. That is a PRODUCT PROBE with
    # a string predicate bolted on the end, and exempting it is exactly the
    # blind spot documented for `isinstance` above -- except this one was
    # reachable by an idiom somebody might actually write, since chaining off a
    # lookup is natural.
    #
    # So: exempt a text predicate only when nothing in its receiver chain is
    # itself a non-environmental call. `s.startswith(...)` stays exempt;
    # `lookup().startswith(...)` does not.
    if isinstance(call.func, ast.Attribute) and call.func.attr in TEXT_PREDICATES:
        return _text_predicate_is_value_in_hand(
            call, pathlike, index, line, seen, guard)
    # Constructing a filesystem path is not a probe of the product. This matters
    # because provenance chases `p = Path(...)` back from `if not p.exists():`,
    # and the factory call would otherwise be scored as a product probe and drag
    # the whole condition out of the environment allowlist.
    if name in PATH_FACTORIES:
        return True
    if isinstance(call.func, ast.Attribute) and call.func.attr in PATH_METHODS:
        receiver = _dotted(call.func.value)
        root = receiver.split(".", 1)[0] if receiver else None
        if root in pathlike:
            return True
        if isinstance(call.func.value, ast.Call) and _call_name(call.func.value) in PATH_FACTORIES:
            return True
    return False


def _condition_provenance(
    test: ast.expr,
    index: _AssignmentIndex,
    line: int,
    pathlike: Set[str],
    depth: int = 0,
    guard: Optional[ast.stmt] = None,
) -> _Provenance:
    prov = _Provenance()
    seen_names: Set[str] = set()

    # A condition whose OUTERMOST predicate asks about a value's TYPE is settled
    # right here, whatever the value's provenance. `not isinstance(value, str)`
    # is a type filter; it does not go looking for anything, so its failure
    # branch cannot mean "the product could not be driven" -- even when `value`
    # traces back to a call, which is what dragged the real case in: provenance
    # chased `value = rec.get(key)` and scored `.get` as a product probe.
    outer = test.operand if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not) else test
    if isinstance(outer, ast.Call) and _call_name(outer) in VALUE_PREDICATES:
        prov.has_call = True
        prov.all_calls_environmental = True
        return prov
    # GitHub #305. Same shortcut for a string predicate on a value in hand:
    # `s.startswith("#")` is settled here regardless of where `s` came from,
    # because asking what a string BEGINS WITH cannot be a probe of whether the
    # product could be driven.
    if (isinstance(outer, ast.Call) and isinstance(outer.func, ast.Attribute)
            and outer.func.attr in TEXT_PREDICATES
            # Same restriction as _classify_call, via the SAME helper -- which
            # checks the receiver AND the arguments, because the unbound form
            # `str.startswith(value, "P")` puts the value in an argument. This
            # shortcut returns before the provenance walk runs, so any gap here
            # reopens the hole regardless of what the classifier does.
            and _text_predicate_is_value_in_hand(
                outer, pathlike, index, line, None, guard)):
        prov.has_call = True
        prov.all_calls_environmental = True
        return prov

    def visit(node: ast.AST, depth: int) -> None:
        if depth > 4:
            return
        for sub in ast.walk(node):
            if isinstance(sub, ast.Attribute):
                dotted = _dotted(sub)
                if dotted in ENV_ATTRS or (dotted and dotted.split(".")[0:2] == ["os", "environ"]):
                    prov.saw_env_attr = True
            if isinstance(sub, ast.Subscript):
                dotted = _dotted(sub.value)
                if dotted in ENV_ATTRS:
                    prov.saw_env_attr = True
            if isinstance(sub, ast.Call):
                prov.has_call = True
                if not _classify_call(sub, pathlike, index, line, None, guard):
                    prov.all_calls_environmental = False
            elif isinstance(sub, ast.Name) and sub.id not in seen_names:
                seen_names.add(sub.id)
                # GitHub #305, NINTH correction. This chase used to take
                # `latest_before` -- ONE binding, the textually last. The
                # multiplicity work of corrections 6-8 went into the
                # value-in-hand classifier only, so it closed the conditional
                # and try/except rebinding false greens for guards wrapped in a
                # text predicate and left them WIDE OPEN for the canonical
                # WIN-45 shape, which has no predicate at all:
                #
                #     if cond:
                #         w = session.find_window()
                #     else:
                #         w = None
                #     if w is None:
                #         pytest.skip("not reachable")      -> 0 findings
                #
                # Swap the branches and it flags. Truthiness, `!=` and
                # try/except spellings all measured the same. Found by the p98
                # phase-3 claim-check gate, which observed that the fix had been
                # wired into one of the two places that needed it.
                #
                # Visiting EVERY reaching binding can only add calls to the
                # provenance, never remove one, so the direction is toward
                # reporting.
                if guard is not None:
                    sources = index.reaching(sub.id, guard)
                else:
                    sources = index.all_before(sub.id, None)
                for source in sources:
                    if source is not None:
                        visit(source, depth + 1)

    visit(test, depth)
    return prov


# --------------------------------------------------------------------------
# scanning
# --------------------------------------------------------------------------


def _condition_text(source_lines: Sequence[str], node: ast.expr) -> str:
    try:
        text = ast.unparse(node)  # Python 3.9+
    except Exception:  # pragma: no cover - defensive
        lineno = getattr(node, "lineno", 1)
        text = source_lines[lineno - 1].strip() if lineno - 1 < len(source_lines) else "<?>"
    text = " ".join(text.split())
    return text if len(text) <= 160 else text[:157] + "..."


def scan_source(source: str, filename: str) -> List[Finding]:
    """Return every structurally-matching guard in `source`."""
    # core.autocrlf=true: tracked files are CRLF on disk. ast handles it, but the
    # line-slicing fallback and any caller diffing text must see LF.
    source = source.replace("\r\n", "\n").replace("\r", "\n")
    try:
        tree = ast.parse(source, filename=filename)
    except SyntaxError as exc:
        raise ValueError("%s: %s" % (filename, exc)) from exc
    lines = source.split("\n")
    findings: List[Finding] = []

    # Module-scope constants matter: `FIXTURE_SINGLE = Path(...)` at the top of
    # the file feeds `if not FIXTURE_SINGLE.exists(): pytest.skip(...)` inside a
    # test, and without this the receiver is untyped and the fixture gate reads
    # as a product probe.
    module_index = _AssignmentIndex(tree, recurse_into_functions=False)
    module_pathlike = module_index.pathlike_names()

    for func in ast.walk(tree):
        if not isinstance(func, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        index = _AssignmentIndex(func)
        pathlike = index.pathlike_names(seed=module_pathlike)
        in_test = func.name.startswith("test_")
        asserted = _deferred_failure_names(func)
        for node in ast.walk(func):
            if not isinstance(node, ast.If):
                continue
            for branch in (node.body, node.orelse):
                if not branch:
                    continue
                # An `elif` chain: the orelse is a single nested If, handled on
                # its own iteration of the outer walk.
                if len(branch) == 1 and isinstance(branch[0], ast.If) and branch is node.orelse:
                    continue
                exit_kind = _branch_exit_kind(branch, in_test)
                if exit_kind is None:
                    continue  # (a) fails: no early exit
                if _branch_fails(branch):
                    continue  # (b) fails: the branch DOES fail the test
                if _branch_defers_failure(branch, asserted):
                    continue  # (b) fails: deferred failure via an asserted accumulator
                # `guard=node`: the exemption's binding chase needs the guard
                # STATEMENT, not its line, to compute reaching definitions.
                prov = _condition_provenance(
                    node.test, index, node.lineno, pathlike, guard=node)
                if not prov.has_call:
                    continue  # (c) fails: static predicate, not a runtime probe
                if prov.saw_env_attr or prov.all_calls_environmental:
                    continue  # (d) fails: environment gate
                findings.append(
                    Finding(
                        file=filename,
                        line=node.lineno,
                        function=func.name,
                        exit_kind=exit_kind,
                        condition=_condition_text(lines, node.test),
                    )
                )
                break  # one finding per `if`, even if both arms match
    return findings


def scan_path(path: Path, root: Optional[Path] = None) -> List[Finding]:
    files: List[Path] = []
    if path.is_dir():
        for candidate in sorted(path.rglob("*.py")):
            # Relative to the SCAN ROOT, not absolute: the self-tests' own
            # temp trees live under `tests/_tmp/<name>/tests/`, so matching on
            # absolute parts would exclude the very fixtures they scan.
            parts = set(candidate.relative_to(path).parts)
            # Mirrors pytest.ini's `norecursedirs`. `tests/_tmp` in particular is
            # this repo's `--basetemp`, so THIS FILE's own self-tests write
            # offending fixtures there; without the exclusion the checker scans
            # its own scratch output and reports two phantom new findings.
            if parts & {
                "__pycache__",
                "_runtime",
                "_tmp",
                "build",
                "out",
                "vcpkg_installed",
                "external",
                ".git",
            }:
                continue
            # Same reason, by PREFIX rather than exact name: `tests/_gtest_*/`
            # is this repo's convention for a gtest/UI run's scratch output and
            # is gitignored as a pattern (.gitignore:662), so such a directory
            # can never hold tracked code -- what it holds is COPIES of test
            # files the checker already scans in their real location. Without
            # this, a local run reports phantom new findings for whatever
            # scratch happens to be lying around and disagrees with CI, where a
            # clean checkout has none of these directories. Measured: four
            # phantom findings from one `tests/_gtest_209proof/` copy
            # (GitHub #204 work, 2026-09-18).
            if any(part.startswith("_gtest_") for part in parts):
                continue
            # Directory walks look at test modules and conftests only. A CLI
            # helper such as tests/visual/update_baselines.py has `return`s that
            # are ordinary control flow and no test to falsely green.
            if not (candidate.name.startswith("test_") or candidate.name == "conftest.py"):
                continue
            files.append(candidate)
    elif path.suffix == ".py":
        files.append(path)

    findings: List[Finding] = []
    for candidate in files:
        with open(candidate, "r", encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        rel = candidate
        if root is not None:
            try:
                rel = candidate.relative_to(root)
            except ValueError:
                pass
        findings.extend(scan_source(source, rel.as_posix()))
    return findings


def fingerprint_findings(findings: Sequence[Finding]) -> List[str]:
    """One fingerprint per finding, in the order given.

    Guards that are identical in file/function/exit_kind/condition are
    distinguished by an ordinal assigned in the iteration order (callers sort by
    file then line), so N identical guards in one file occupy N distinct
    baseline entries and one of them cannot silently stand in for another.
    """
    seen: Dict[str, int] = {}
    out: List[str] = []
    for finding in findings:
        shape = finding.shape()
        ordinal = seen.get(shape, 0)
        seen[shape] = ordinal + 1
        out.append("%s#%d" % (shape, ordinal))
    return out


@dataclass(frozen=True)
class Baseline:
    """The committed ratchet file, as loaded.

    `fingerprints is None` means schema 1 -- the file predates #204 and carries
    no `known_fingerprints`. In that case `sites` is authoritative for both the
    "is this known?" and the "is this stale?" question, which is the
    `check_magnitude_guard.py` shape applied to line keys.
    """

    sites: Set[str]
    fingerprints: Optional[Set[str]]
    data: Dict[str, object]

    @property
    def schema(self) -> int:
        return 1 if self.fingerprints is None else 2

    def is_empty(self) -> bool:
        return not self.sites and not self.fingerprints


EMPTY_BASELINE = Baseline(sites=set(), fingerprints=None, data={})


def load_baseline_file(path: Path) -> Baseline:
    if not path.exists():
        return EMPTY_BASELINE
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    fingerprints = data.get("known_fingerprints")
    return Baseline(
        sites=set(data.get("known_sites", [])),
        fingerprints=None if fingerprints is None else set(fingerprints),
        data=data,
    )


def load_baseline(path: Path) -> Set[str]:
    """Backward-compatible accessor: just the `file:line` site keys."""
    return load_baseline_file(path).sites


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--paths",
        nargs="+",
        default=["tests"],
        help="files or directories to scan (default: tests)",
    )
    parser.add_argument("--root", default=".", help="repo root for relative reporting")
    parser.add_argument("--baseline", default=None, help="ratchet file of already-known sites")
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument(
        "--allow-stale-baseline",
        action="store_true",
        help=(
            "do not fail when a baseline entry matches nothing in the tree. "
            "Downgrades the stale failure ONLY -- an unreviewed new finding "
            "still fails."
        ),
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    findings: List[Finding] = []
    for raw in args.paths:
        target = Path(raw)
        if not target.is_absolute():
            target = root / raw
        if not target.exists():
            sys.stderr.write("path does not exist: %s\n" % target)
            return 2
        findings.extend(scan_path(target, root))

    findings.sort(key=lambda f: (f.file, f.line))
    live_fingerprints = fingerprint_findings(findings)
    live_fingerprint_set = set(live_fingerprints)
    live_keys = {f.key() for f in findings}

    baseline = EMPTY_BASELINE
    if args.baseline:
        baseline = load_baseline_file(Path(args.baseline))

    if baseline.schema == 2:
        new = [
            f
            for f, fp in zip(findings, live_fingerprints)
            if fp not in (baseline.fingerprints or set())
        ]
        stale = sorted(fp for fp in (baseline.fingerprints or set()) if fp not in live_fingerprint_set)
        # Line keys are advisory under schema 2; a drifted one is reported but
        # never fails, and -- crucially -- never absorbs a finding either.
        drift = sorted(k for k in baseline.sites if k not in live_keys)
    else:
        new = [f for f in findings if f.key() not in baseline.sites]
        stale = sorted(k for k in baseline.sites if k not in live_keys)
        drift = []

    if args.update_baseline and args.baseline:
        previously_known = baseline.fingerprints if baseline.schema == 2 else None
        absorbed = (
            [fp for fp in live_fingerprints if fp not in previously_known]
            if previously_known is not None
            else []
        )
        payload: Dict[str, object] = {
            "_comment": (
                "Sites already carrying the WIN-45 reachability-skip statement shape when "
                "check_reachability_skip_shape.py was installed (F29). This file is a RATCHET, "
                "not an endorsement: it stops NEW sites from landing. Shrinking it is the work; "
                "growing it requires a deliberate edit and a reason. `known_fingerprints` is the "
                "authoritative list (see GitHub #204): it carries no line numbers, so an edit "
                "above a guard no longer re-reports it as new. `known_sites` is the "
                "human-readable file:line index of the same entries, regenerated here and NOT "
                "consulted when deciding whether a finding is known."
            ),
            "generated_by": "tools/gate/check_reachability_skip_shape.py --update-baseline",
            "schema": 2,
            "count": len(findings),
            "known_sites": [f.key() for f in findings],
            "known_fingerprints": live_fingerprints,
        }
        # Carry forward operator-authored metadata. `--update-baseline` used to
        # rewrite the payload from scratch, which is how the `_ownership` block
        # recorded on 2026-09-07 was silently deleted by a routine refresh on
        # 2026-09-18 (GitHub #204). Anything the generator does not own itself
        # survives an update now.
        for extra_key, extra_value in baseline.data.items():
            if extra_key.startswith("_") and extra_key not in payload:
                payload[extra_key] = extra_value
        if "_ownership" in baseline.data:
            payload["_ownership"] = baseline.data["_ownership"]
        # Metadata first, the long entry lists last: an ownership block written
        # after 437 fingerprints is one nobody scrolls to.
        preferred = ["_comment", "_ownership"]
        payload = dict(
            [(k, payload[k]) for k in preferred if k in payload]
            + [(k, v) for k, v in payload.items() if k not in preferred]
        )
        with open(args.baseline, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(payload, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        sys.stdout.write("baseline written: %s (%d sites)\n" % (args.baseline, len(findings)))
        if previously_known is not None:
            sys.stdout.write(
                "  absorbed %d site(s) that were NOT previously reviewed; "
                "pruned %d stale entry(ies)\n" % (len(absorbed), len(stale))
            )
            for fp in absorbed:
                sys.stdout.write("  ABSORBED-NEW %s\n" % fp)
        return 0

    if args.as_json:
        sys.stdout.write(
            json.dumps(
                {
                    "total": len(findings),
                    "baseline_known": len(findings) - len(new),
                    "baseline_schema": baseline.schema if args.baseline else None,
                    "stale": stale,
                    "drifted_sites": drift,
                    "new": [asdict(f) for f in new],
                },
                indent=2,
                ensure_ascii=False,
            )
            + "\n"
        )
    else:
        for finding in new:
            sys.stdout.write(
                "%s:%d  [%s in %s]  guard: %s\n"
                % (
                    finding.file,
                    finding.line,
                    finding.exit_kind,
                    finding.function,
                    finding.condition,
                )
            )
        for entry in stale:
            sys.stdout.write("STALE-BASELINE %s\n" % entry)
        for entry in drift:
            sys.stdout.write("DRIFT-SITE %s (line moved; entry still matched by fingerprint)\n" % entry)
        sys.stdout.write(
            "\n%d structurally-matching guard(s); %d known via baseline; %d NEW; "
            "%d STALE baseline entry(ies)%s\n"
            % (
                len(findings),
                len(findings) - len(new),
                len(new),
                len(stale),
                "; %d drifted line key(s)" % len(drift) if drift else "",
            )
        )

    if stale:
        sys.stdout.write(
            "\n%s: %d baseline entry(ies) match nothing in the tree.\n"
            "The guard was removed or rewritten -- good news if it was converted, but the\n"
            "entry must be pruned so it cannot silently re-accept a different guard later.\n"
            "Refresh with:\n"
            "    python tools/gate/check_reachability_skip_shape.py --paths tests \\\n"
            "        --baseline %s --update-baseline\n"
            "and READ the ABSORBED-NEW lines it prints before committing the result.\n"
            % (
                "STALE (downgraded by --allow-stale-baseline)"
                if args.allow_stale_baseline
                else "FAIL",
                len(stale),
                args.baseline or "<baseline>",
            )
        )

    if new:
        sys.stdout.write(
            "\nFAIL: a lookup guard whose failure branch exits without failing the test.\n"
            "That is the WIN-45 false green: the product could not be driven and the run\n"
            "reported not-a-failure. Convert the site to the recorded-non-pass shape:\n"
            "    _reach = (\"<what was not reachable>\")\n"
            "    record_matrix_result(current_test_id(), \"blocked\", _reach)\n"
            "    pytest.fail(f\"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}\")\n"
            "If the guard is a genuine ENVIRONMENT gate, express it as one (os.environ /\n"
            "sys.platform / a filesystem-existence probe) so this checker can see that it is.\n"
        )

    # Mirrors check_magnitude_guard.py:504. `--allow-stale-baseline` downgrades
    # the stale half ONLY: an unreviewed new finding still fails the run.
    if new or (stale and not args.allow_stale_baseline):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
