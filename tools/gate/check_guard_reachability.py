#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Standing control: a guard that no automatic workflow invokes is not a guard.

GitHub #255. This is the second half of test-gap class **C-BELOW** ("every test
enters below the defect", `docs/audit/test-gap-classes.md` section 3).
`check_engine_wiring.py` covers the half where PRODUCT CODE is unreachable from
`src/main.cpp`. It structurally cannot cover this half -- it walks C++ classes
under `src/` -- and this half is the one that has actually recurred:

  * #145  two security scripts "invoked by no workflow"
  * #187  seven gate scripts wired only to dispatch-only `windows-validation.yml`
  * #201  `pdfe_security_nightly` still "invoked by zero workflows", AFTER #145

THREE THINGS THIS CHECKER REFUSES TO DO, each because a real defect did them:

1. **It does not scope the population by the `check_` filename prefix.**
   Four of #187's seven scripts -- `fz_try_raii_scan.py`, `page_load_guard_scan.py`,
   `parity_filter.py`, `scrub_artifacts.py` -- are not named `check_*`. A
   prefix-scoped control would have caught 3 of 7 in the very defect it exists
   to prevent. Defining a population by a naming convention rather than by the
   property is test-gap class **C-ENUM**, and it is how this project keeps
   rebuilding the same blind spot.

2. **It does not count "named in a workflow" as reachable.** #187's seven WERE
   named -- in `windows-validation.yml`, which is `workflow_dispatch`-only, so
   they ran on neither push nor locally. Automatic means push / pull_request /
   pull_request_target, and that list is IMPORTED from
   `check_workflow_triggers.py` rather than restated here, because a rule
   implemented twice with one copy fixed is DCA `0052`.

3. **It does not report "could not measure" as a pass.** A workflow whose YAML
   will not parse is a hard error naming the file. Healing an unmeasurable
   condition to green is test-gap class **C-FAILOPEN** (#246).

A MENTION IS NOT AN INVOCATION. `check_engine_wiring.py`'s first draft counted
a class named inside a `//` comment as a call site and was failed by a gate.
The analogous traps here are all handled: a script named in a YAML comment (the
parser drops those), in a `#` comment inside a `run:` shell block, inside
another script's docstring, or in a step hidden behind a literally-false `if:`.

REACHABILITY IS TRANSITIVE. A guard invoked by a pytest that itself runs on
push is reached; so is one invoked by another already-reached guard. Measuring
only direct invocation would flag `scan_binary_path_hygiene.py`, which is
genuinely reached via `test_check_f45_ci_only_markers.py`.

KNOWN LIMIT, stated rather than hidden: only a LITERALLY false `if:` condition
(`false`, `${{ false }}`) marks a step dead. A condition that is false for
other reasons -- an expression over a variable, a matrix that never matches --
is treated as live. Narrowing that would need an expression evaluator; the
honest position is that this checker catches the literal case and says so.

Usage:
    python tools/gate/check_guard_reachability.py [--repo-root .]
    python tools/gate/check_guard_reachability.py --self-test
"""
from __future__ import annotations

import argparse
import glob
import importlib.util
import io
import json
import os
import re
import sys
import tempfile
import ast
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))

BASELINE_REL = os.path.join("tools", "gate", "guard_reachability_baseline.json")
GATE_REL = os.path.join("tools", "gate")
WORKFLOW_REL = os.path.join(".github", "workflows")

#: Literal false conditions. Deliberately tiny -- see KNOWN LIMIT above.
ALWAYS_FALSE = re.compile(r"^\s*(?:false|\$\{\{\s*false\s*\}\})\s*$", re.I)

#: What a baseline entry may BE. Enforced by `load_baseline`, which is what
#: stops a live gap being silenced under an invented label.
#:
#: Each kind is described at its own line below and NOWHERE ELSE, deliberately.
#: This comment used to open by stating how many kinds there were and naming
#: them -- then another was added and the sentence silently became false. The
#: phase-3 claim-check gate caught it as the third instance of one shape in
#: this pipeline: prose describing a mechanism that had since moved. A summary
#: that restates the set is a second copy of it, and the copy is what goes
#: stale. So there is no tally here and no list here; the tuple is the list.
#:
#: `test_the_taxonomy_is_not_summarised_anywhere_above_its_definition` enforces
#: that, and it reddened twice on drafts of this very comment -- once for
#: naming a kind while explaining the history, once for quoting the old
#: sentence verbatim. Both were the rule working, not the rule being fussy.
BASELINE_KINDS = frozenset((
    # Archival: machine-checked one pipeline phase at one moment and is kept
    # as evidence. Re-running it on a later tree would describe a state that
    # no longer exists.
    "one-shot-phase-verifier",
    # A REAL orphan that is parked. MUST carry an issue number -- being listed
    # here is not a resolution, and without an issue it is simply forgotten.
    "admitted-gap",
    # A script whose JOB is to run outside CI -- the local mirror of the CI
    # suite. Invoking it from the workflow it mirrors would be circular. This
    # kind is deliberately narrow: it is not "we run it by hand sometimes",
    # which is an admitted-gap.
    "deliberately-local-only",
    # Reached, but through a surface this census does not model: a CMake
    # `add_test` target, which needs a CONFIGURED BUILD TREE and so cannot be
    # a fast-checks step. The census walks workflow files, so such a guard
    # looks orphaned while actually being enforced on every ctest lane.
    #
    # This is NOT an amnesty and is the opposite of an admitted-gap: the entry
    # MUST name the `add_test` target, and `_validate_ctest_reachable` then
    # confirms that target really exists in a CMakeLists under the repo. A
    # claim that cannot be checked would just be an orphan with better prose
    # -- which is the exact shape this guard exists to catch.
    "reached-via-ctest",
))


class GuardReachabilityError(Exception):
    """Raised when the property cannot be MEASURED. Never caught into a pass."""


# --------------------------------------------------------------------------
# reused rules -- imported, never restated
# --------------------------------------------------------------------------

def _triggers_module():
    """Import `check_workflow_triggers` for AUTO_TRIGGERS and its `on:` parser.

    That module already owns the automatic-vs-manual distinction, including the
    YAML gotcha that a bare `on:` key parses as the boolean True. Restating any
    of it here would create the two-copy divergence DCA `0052` describes.
    """
    path = os.path.join(HERE, "check_workflow_triggers.py")
    if not os.path.exists(path):
        raise GuardReachabilityError(
            "cannot measure: %s is missing, so the automatic-trigger rule "
            "cannot be imported" % path)
    spec = importlib.util.spec_from_file_location("_cwt", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def strip_python(text, what="<python>"):
    """Reference text of a Python file: invocations kept, prose discarded.

    The distinction that matters here is finer than "strip strings", and
    getting it wrong breaks the checker in one direction or the other:

      * A guard named in a **docstring** is PROSE -- a cross-reference, a
        header note. Counting it as an invocation is the mistake
        `check_engine_wiring.py`'s first draft made with `//` comments, and a
        gate failed it for exactly that.
      * A guard named in an ordinary **string literal** is usually the
        INVOCATION itself, because that is how Python spawns one:
        `subprocess.run(["python", "tools/gate/parity_filter.py"])`. Discarding
        those would make every indirect route invisible and the checker would
        under-report reachability -- flagging guards that genuinely run.

    `tokenize` cannot tell the two apart; `ast` can, because a docstring is
    definitionally the first statement of a module, class or function. So the
    reference text is every name, attribute and NON-docstring string constant.

    Fails closed: a file that will not parse raises rather than reading as
    empty, since an empty blob would silently mean "references nothing".
    """
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError) as exc:
        raise GuardReachabilityError(
            "cannot measure: %s will not parse (%s)" % (what, str(exc)[:120]))

    docstrings = set()
    for node in ast.walk(tree):
        body = getattr(node, "body", None)
        if not isinstance(node, (ast.Module, ast.ClassDef,
                                 ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        if (body and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)):
            docstrings.add(id(body[0].value))

    parts = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if id(node) not in docstrings:
                parts.append(node.value)
        elif isinstance(node, ast.Name):
            parts.append(node.id)
        elif isinstance(node, ast.Attribute):
            parts.append(node.attr)
        elif isinstance(node, ast.Import):
            parts.extend(a.name for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            # `from x import y` -- both halves can name a guard module.
            if node.module:
                parts.append(node.module)
            parts.extend(a.name for a in node.names)
    return "\n".join(parts)


def mentions(blob, basename):
    """Does `blob` reference the guard `basename` as code?

    Two spellings count, and missing either one produces a FALSE POSITIVE --
    a guard reported as unreachable while it genuinely runs:

      * the filename, `fz_try_raii_scan.py`, as a workflow invokes it;
      * the bare module stem, `fz_try_raii_scan`, as Python imports it
        (`import fz_try_raii_scan as scan`) and as `python -m` spells it.

    BOTH are anchored at the left, and that is load-bearing. A plain substring
    test credits `check_guard_reachability.py` whenever it sees
    `test_check_guard_reachability.py` -- so merely naming a guard's COMPANION
    TEST in the pytest batch made the guard itself read as reached. Every guard
    in this repo has such a companion, so the plain test quietly vouched for
    all of them. Found by the phase-2 claim-check gate, which deleted this
    guard's own workflow step and watched it still report itself reached.

    `(?<![\\w.-])` rejects a match preceded by a word character, a dot or a
    hyphen, so `test_foo.py` and `my-foo.py` no longer stand in for `foo.py`,
    while `tools/gate/foo.py` and a bare `foo` still match.
    """
    stem = re.escape(os.path.splitext(basename)[0])
    if re.search(r"(?<![\w.-])%s\.py" % stem, blob):
        return True
    return re.search(r"(?<![\w.-])%s\b" % stem, blob) is not None


def strip_shell_comments(text):
    """Drop `#`-to-end-of-line from a shell `run:` block.

    YAML comments are already gone -- the parser removed them -- but a `#`
    inside a `run:` block scalar is shell, not YAML, and survives parsing. A
    guard named only in a shell comment is a mention, not an invocation.
    """
    out = []
    for line in text.splitlines():
        quote = None
        cut = len(line)
        for i, ch in enumerate(line):
            if quote:
                if ch == quote:
                    quote = None
            elif ch in "'\"":
                quote = ch
            elif ch == "#" and (i == 0 or line[i - 1].isspace()):
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


# --------------------------------------------------------------------------
# population -- derived from the tree
# --------------------------------------------------------------------------

def discover_guards(root="."):
    """{basename: relpath} for every invocable guard under tools/gate.

    Globbed from the filesystem at run time. There is deliberately no hardcoded
    filename list and no `check_` prefix filter; see refusal 1 in the module
    docstring. `test_*.py` companions are excluded because they are run BY the
    pytest batch rather than being guards in their own right, and anything under
    `fixtures/` is test data.
    """
    gate_dir = os.path.join(root, GATE_REL)
    found = {}
    for path in sorted(glob.glob(os.path.join(gate_dir, "*.py"))):
        base = os.path.basename(path)
        if base.startswith("test_"):
            continue
        found[base] = os.path.relpath(path, root).replace("\\", "/")
    return found


def discover_companions(root="."):
    """{basename: relpath} for the tools/gate pytest companions."""
    gate_dir = os.path.join(root, GATE_REL)
    return {os.path.basename(p): os.path.relpath(p, root).replace("\\", "/")
            for p in sorted(glob.glob(os.path.join(gate_dir, "test_*.py")))}


# --------------------------------------------------------------------------
# workflows -- what an automatic trigger actually executes
# --------------------------------------------------------------------------

def _live_step_text(node, out):
    """Collect `run`/`with` text from steps, skipping literally-dead ones."""
    if isinstance(node, dict):
        cond = node.get("if")
        if cond is not None and ALWAYS_FALSE.match(str(cond)):
            return
        for key, val in node.items():
            if key == "if":
                continue
            if key == "run" and isinstance(val, str):
                out.append(strip_shell_comments(val))
            elif key == "with" and isinstance(val, dict):
                for wv in val.values():
                    if isinstance(wv, str):
                        out.append(strip_shell_comments(wv))
            else:
                _live_step_text(val, out)
    elif isinstance(node, list):
        for item in node:
            _live_step_text(item, out)


def automatic_invocation_text(root="."):
    """Concatenated live `run:` text of every automatically-triggered workflow.

    Raises GuardReachabilityError if any workflow will not parse -- refusal 3.
    """
    cwt = _triggers_module()
    wf_dir = os.path.join(root, WORKFLOW_REL)
    if not os.path.isdir(wf_dir):
        raise GuardReachabilityError(
            "cannot measure: no workflow directory at %s" % wf_dir)

    scanned = cwt.scan(wf_dir)
    broken = sorted(n for n, v in scanned.items() if "error" in v)
    if broken:
        raise GuardReachabilityError(
            "cannot measure: workflow YAML will not parse: %s (%s)"
            % (", ".join(broken), scanned[broken[0]]["error"]))

    auto_names = sorted(n for n, v in scanned.items() if v.get("auto"))
    if not auto_names:
        raise GuardReachabilityError(
            "cannot measure: no workflow has an automatic trigger (%s). Either "
            "the repository has no per-commit gate, or the `on:` blocks did not "
            "parse as expected." % "/".join(cwt.AUTO_TRIGGERS))

    import yaml
    chunks = []
    for name in auto_names:
        path = os.path.join(wf_dir, name)
        doc = yaml.safe_load(io.open(path, encoding="utf-8"))
        _live_step_text(doc, chunks)
    return auto_names, "\n".join(chunks)


# --------------------------------------------------------------------------
# reachability
# --------------------------------------------------------------------------

def compute(root="."):
    """Return (auto_names, population, reached, unreached).

    Seeds are guards named in the live text of an automatic workflow. The set
    then closes transitively over two indirect routes: a pytest companion the
    workflow runs, and an already-reached guard that invokes another.
    """
    population = discover_guards(root)
    companions = discover_companions(root)
    auto_names, auto_text = automatic_invocation_text(root)

    def names_in(blob, candidates):
        return {b for b in candidates if mentions(blob, b)}

    reached = names_in(auto_text, population)

    # A companion the automatic workflow runs is itself a live invoker.
    live_companions = names_in(auto_text, companions)

    frontier = sorted(reached) + sorted(live_companions)
    seen_sources = set()
    while frontier:
        base = frontier.pop()
        if base in seen_sources:
            continue
        seen_sources.add(base)
        path = population.get(base) or companions.get(base)
        if not path:
            continue
        full = os.path.join(root, path)
        if not os.path.exists(full):
            continue
        body = strip_python(io.open(full, encoding="utf-8", errors="replace").read(),
                            what=path)
        for found in names_in(body, population):
            if found == base or found in reached:
                continue
            # A guard is NOT reached by its own companion test. `test_foo.py`
            # names `foo.py` because it TESTS it -- every such file does, if
            # only to import the module under test. Counting that as an
            # invocation lets a guard vouch for itself, which is test-gap class
            # C-GUARDSELF: the auditor's reference is not independent of the
            # thing it audits. Another guard's test naming it still counts,
            # because that is a genuine second party.
            if base == "test_%s" % found:
                continue
            reached.add(found)
            frontier.append(found)

    unreached = {b: p for b, p in population.items() if b not in reached}
    return auto_names, population, reached, unreached


# --------------------------------------------------------------------------
# baseline -- a ratchet, not an amnesty
# --------------------------------------------------------------------------

def _census(root="."):
    """The `_known_guards` census, or None when the baseline has none.

    GitHub #251. Read separately because `load_baseline` strips every
    `_`-prefixed key -- which is exactly how the taxonomy in `_rules` came to be
    prose nothing read (DCA 0093). Returning None rather than an empty set
    matters: "no census recorded" and "a census listing nothing" must not be
    the same answer, or removing the key would silently disable the check.
    """
    path = os.path.join(root, BASELINE_REL)
    if not os.path.exists(path):
        return None
    try:
        data = json.load(io.open(path, encoding="utf-8"))
    except ValueError:
        return None
    known = data.get("_known_guards")
    if not isinstance(known, list):
        return None
    return sorted(str(x) for x in known)


def load_baseline(root="."):
    path = os.path.join(root, BASELINE_REL)
    if not os.path.exists(path):
        raise GuardReachabilityError("cannot measure: baseline missing at %s" % path)
    try:
        data = json.load(io.open(path, encoding="utf-8"))
    except ValueError as exc:
        raise GuardReachabilityError("cannot measure: baseline is not valid JSON (%s)" % exc)
    entries = {k: v for k, v in data.items() if not k.startswith("_")}
    for name, ent in sorted(entries.items()):
        if not isinstance(ent, dict):
            raise GuardReachabilityError(
                "baseline entry %r must be an object with 'kind' and 'reason'" % name)
        for field in ("kind", "reason"):
            val = str(ent.get(field, "")).strip()
            if not val:
                raise GuardReachabilityError(
                    "baseline entry %r has no %s. An unexplained entry is an "
                    "amnesty, not a ratchet." % (name, field))
        if len(str(ent["reason"]).strip()) < 20 or "TODO" in str(ent["reason"]).upper():
            raise GuardReachabilityError(
                "baseline entry %r has a placeholder reason (%r)"
                % (name, ent["reason"]))

        # The taxonomy is ENFORCED, not merely documented. `_rules` describes
        # two kinds and says an admitted gap must carry an issue; until the
        # phase-1 claim-check gate, both were prose a human had to honour,
        # which is the "documentation claims a control that does not exist"
        # shape this whole guard exists to catch.
        kind = str(ent["kind"]).strip()
        if kind not in BASELINE_KINDS:
            raise GuardReachabilityError(
                "baseline entry %r has kind %r, which is outside the taxonomy "
                "%s. An unrecognised kind lets a live gap be parked under a "
                "label nothing checks." % (name, kind, sorted(BASELINE_KINDS)))
        if kind == "admitted-gap" and not str(ent.get("issue", "")).strip():
            raise GuardReachabilityError(
                "baseline entry %r is an admitted-gap with no 'issue'. An "
                "admitted gap is not resolved by being listed here; without an "
                "issue number it is simply forgotten." % name)
        if kind == "reached-via-ctest":
            _validate_ctest_reachable(root, name, ent)
    return entries


def _validate_ctest_reachable(root, name, ent):
    """Confirm a `reached-via-ctest` entry's named target actually exists.

    The whole value of this kind is that the claim is MACHINE-CHECKED. Without
    this, "it is reached from ctest" would be prose, and a guard could be
    parked under it forever after the `add_test` was renamed or deleted -- the
    stale-baseline failure the `_rules` ratchet is built to prevent, arriving
    through a new door.
    """
    target = str(ent.get("add_test", "")).strip()
    if not target:
        raise GuardReachabilityError(
            "baseline entry %r is reached-via-ctest with no 'add_test'. The "
            "kind exists because the claim is checkable; name the target."
            % name)
    needle = "NAME %s" % target
    for cml in Path(root).rglob("CMakeLists.txt"):
        parts = cml.parts
        if "build" in parts or "_tmp" in parts or "_runtime" in parts:
            continue
        try:
            text = cml.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        # Match `add_test(NAME <target>` across the line break CMake allows,
        # by normalising whitespace rather than guessing the formatting.
        if needle in " ".join(text.split()):
            return
    raise GuardReachabilityError(
        "baseline entry %r claims reached-via-ctest through add_test target "
        "%r, but no CMakeLists.txt under the repo declares that target. Either "
        "the target was renamed or deleted, or the claim was never true -- "
        "both mean this guard is NOT reached." % (name, target))


def check(root="."):
    """Return (exit_code, [lines])."""
    lines = []
    try:
        auto_names, population, reached, unreached = compute(root)
        baseline = load_baseline(root)
    except GuardReachabilityError as exc:
        return 1, ["[guard-reachability] ERROR: %s" % exc]

    novel = sorted(set(unreached) - set(baseline))

    lines.append(
        "[guard-reachability] %d guard(s) under tools/gate; %d reached from %s; "
        "%d baselined; %d unreached and not baselined."
        % (len(population), len(reached), "/".join(auto_names),
           len(baseline), len(novel)))

    failures = []

    if novel:
        failures.append(
            "%d guard(s) run on neither push nor locally and are not baselined "
            "-- this is the #187 shape:\n%s"
            % (len(novel), "\n".join("    %s" % unreached[n] for n in novel)))

    healed = sorted(set(baseline) & set(reached))
    if healed:
        failures.append(
            "%d baselined guard(s) are now REACHED and must be removed from %s. "
            "A list that only grows reports 'closed' about itself rather than "
            "about the property:\n%s"
            % (len(healed), BASELINE_REL, "\n".join("    %s" % h for h in healed)))

    gone = sorted(set(baseline) - set(population))
    if gone:
        failures.append(
            "%d baselined guard(s) no longer exist and must be removed from %s:\n%s"
            % (len(gone), BASELINE_REL, "\n".join("    %s" % g for g in gone)))

    # GitHub #251 -- SCOPING, so the next reader does not add a third check.
    #
    # Guard deletion is ALREADY covered in one direction, by two things:
    #   * the `gone` check immediately above -- `set(baseline) - set(population)`
    #     -- which fails when a BASELINED guard stops existing; and
    #   * its self-test case `ratchet-deleted-entry-must-be-removed` (case 10),
    #     which pins that behaviour.
    # Neither is duplicated here.
    #
    # What they do NOT cover is the other 61 guards.
    # A guard that was REACHED -- the healthy majority -- could be deleted along
    # with its workflow step and leave no trace at all: it simply stops being
    # discovered, and a population derived from the filesystem cannot miss what
    # is no longer on the filesystem. That is C-GUARDSELF exactly (the auditor's
    # reference comes from the artifact it audits), and it is why the census
    # below is an INDEPENDENT expected-set rather than another derived one.
    #
    # Both directions are enforced, deliberately: a vanished guard must be
    # removed from the census consciously, and a new guard must be added to it.
    # A one-directional list rots into a list of things that used to exist.
    census = _census(root)
    if census is not None:
        vanished = sorted(set(census) - set(population))
        if vanished:
            failures.append(
                "%d guard(s) in the census no longer exist:\n%s\n"
                "    If the deletion is intended, remove them from `_known_guards` "
                "in %s in the SAME commit. Deleting a guard and its workflow step "
                "together is otherwise invisible -- nothing else in this checker "
                "would notice."
                % (len(vanished), "\n".join("    %s" % g for g in vanished),
                   BASELINE_REL))
        unlisted = sorted(set(population) - set(census))
        if unlisted:
            failures.append(
                "%d guard(s) exist but are absent from the census:\n%s\n"
                "    Add them to `_known_guards` in %s. A census that only ever "
                "loses entries stops being able to notice a loss."
                % (len(unlisted), "\n".join("    %s" % g for g in unlisted),
                   BASELINE_REL))

    if failures:
        lines.append("[guard-reachability] FAIL")
        for f in failures:
            lines.append("  " + f)
        return 1, lines

    lines.append("[guard-reachability] OK -- every guard under tools/gate is "
                 "reachable from an automatic trigger, or baselined with a reason.")
    return 0, lines


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

_WF_AUTO = """\
name: auto
on:
  push:
    branches: [master]
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
%s
"""

_WF_DISPATCH = """\
name: manual
on:
  workflow_dispatch:
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
%s
"""


def _baseline(**entries):
    """A synthetic baseline. Defaults to a well-formed archival entry."""
    # `_tree` always copies check_workflow_triggers.py in, because the imported
    # trigger rule must be present for a tree to be measurable at all. It is
    # genuinely unreached there, so every synthetic baseline accounts for it --
    # otherwise the well-formed case fails on a fixture artefact rather than on
    # the property under test.
    out = {
        "_what": "synthetic",
        "check_workflow_triggers.py": {
            "kind": "one-shot-phase-verifier",
            "reason": "fixture scaffolding: copied in so the imported trigger "
                      "rule exists in the synthetic tree",
        },
    }
    for name, ent in entries.items():
        key = name.replace("__", ".")
        out[key] = ent if ent is not None else {
            "kind": "one-shot-phase-verifier",
            "reason": "synthetic archival entry, long enough to pass the "
                      "placeholder-reason check",
        }
    return out


def _tree(base, workflows, guards, companions=None, baseline=None):
    """Build a synthetic repo. Never touches the live tree."""
    wf = os.path.join(base, WORKFLOW_REL)
    gate = os.path.join(base, GATE_REL)
    os.makedirs(wf)
    os.makedirs(gate)
    for name, text in workflows.items():
        io.open(os.path.join(wf, name), "w", encoding="utf-8").write(text)
    for name, text in {**guards, **(companions or {})}.items():
        io.open(os.path.join(gate, name), "w", encoding="utf-8").write(text)
    # the imported rule must be present for the tree to be measurable at all
    io.open(os.path.join(gate, "check_workflow_triggers.py"), "w",
            encoding="utf-8").write(
        io.open(os.path.join(HERE, "check_workflow_triggers.py"),
                encoding="utf-8").read())
    io.open(os.path.join(base, BASELINE_REL), "w", encoding="utf-8").write(
        json.dumps(baseline if baseline is not None else {"_what": "synthetic"}))
    return base


def _step(cmd, cond=None):
    """Emit a step whose `run:` is a BLOCK scalar.

    The block scalar is load-bearing, not style. With a PLAIN scalar, PyYAML
    strips a trailing ` # comment` itself, per the YAML spec — so the
    comment-mention case below would pass without `strip_shell_comments` ever
    being called, and no-opping that function would leave it green. That is a
    vacuous oracle, and it is what this fixture used to be until the phase-1
    claim-check gate caught it. Every real workflow step in `fast-checks.yml`
    uses `run: |`, so the block scalar is also the shape that actually ships.
    """
    s = "      - name: s\n"
    if cond is not None:
        s += "        if: %s\n" % cond
    s += "        run: |\n"
    for line in cmd.splitlines():
        s += "          %s\n" % line
    return s


def _unreached_in(base):
    _, _, _, unreached = compute(base)
    return set(unreached)


def self_test():
    results = []

    def case(label, ok, detail=""):
        results.append((label, bool(ok), detail))

    stub = "print('x')\n"

    # 1 -- a guard NOT named check_* is in the population at all.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub, "parity_filter.py": stub})
        pop = set(discover_guards(d))
        case("non-check-prefixed-guard-in-population",
             "parity_filter.py" in pop, sorted(pop))

    # 1b -- referenced ONLY as a bare `import` stem, with no `.py` anywhere.
    # The first version of this checker matched the filename only and so
    # reported fz_try_raii_scan.py and select_f35_tranche.py as orphans while
    # both genuinely run. A checker that over-reports is not "safely strict":
    # it trains the reader to add baseline entries for guards that work.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("pytest tools/gate/test_check_a.py")},
              {"parity_filter.py": stub},
              {"test_check_a.py": "import parity_filter as pf\n"})
        synthetic_ok = "parity_filter.py" not in _unreached_in(d)

    # ...and the same assertion against the REAL pair it was missed on, so the
    # case cannot pass on a synthetic tree while the live spelling regresses.
    live_ok, live_detail = True, "live pair absent, synthetic only"
    if os.path.exists(os.path.join(HERE, "select_f35_tranche.py")):
        repo = os.path.dirname(os.path.dirname(HERE))
        try:
            live_ok = "select_f35_tranche.py" not in _unreached_in(repo)
            live_detail = "select_f35_tranche.py reached=%s" % live_ok
        except GuardReachabilityError as exc:
            live_ok, live_detail = False, str(exc)[:120]
    case("import-stem-reachability", synthetic_ok and live_ok, live_detail)

    # 2 -- named ONLY by a dispatch-only workflow. This is #187 exactly.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py"),
                  "m.yml": _WF_DISPATCH % _step("python tools/gate/parity_filter.py")},
              {"check_a.py": stub, "parity_filter.py": stub})
        case("dispatch-only-wiring-not-reachable",
             "parity_filter.py" in _unreached_in(d))

    # 3 -- named only in a shell comment inside a live run: block.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step(
                    "python tools/gate/check_a.py  # also parity_filter.py")},
              {"check_a.py": stub, "parity_filter.py": stub})
        case("comment-mention-not-reachable",
             "parity_filter.py" in _unreached_in(d))

    # 4 -- named only inside a reached guard's DOCSTRING.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": '"""See parity_filter.py for the other half."""\n',
               "parity_filter.py": stub})
        case("docstring-mention-not-reachable",
             "parity_filter.py" in _unreached_in(d))

    # 5 -- invoked, but from a step whose if: is literally false.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % (
                    _step("python tools/gate/check_a.py")
                    + _step("python tools/gate/parity_filter.py", cond="${{ false }}"))},
              {"check_a.py": stub, "parity_filter.py": stub})
        case("always-false-if-not-reachable",
             "parity_filter.py" in _unreached_in(d))

    # 6 -- reached only via a pytest companion the workflow runs.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("pytest tools/gate/test_check_a.py")},
              {"parity_filter.py": stub},
              {"test_check_a.py": "import subprocess\n"
                                  "subprocess.run(['python','tools/gate/parity_filter.py'])\n"})
        case("indirect-via-pytest-reachable",
             "parity_filter.py" not in _unreached_in(d))

    # 7 -- reached only via another guard that is itself reached.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": "import subprocess\n"
                             "subprocess.run(['python','tools/gate/parity_filter.py'])\n",
               "parity_filter.py": stub})
        case("indirect-via-reached-guard-reachable",
             "parity_filter.py" not in _unreached_in(d))

    # 8 -- unparseable YAML is an ERROR, never a pass. Class C-FAILOPEN.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py"),
                  "broken.yml": "on: [push]\njobs:\n  j:\n    steps:\n   - bad\n  x: ["},
              {"check_a.py": stub})
        rc, lines = check(d)
        blob = "\n".join(lines)
        case("unparseable-workflow-is-an-error-not-a-pass",
             rc != 0 and "broken.yml" in blob, blob[:200])

    # 9 -- RATCHET: a baselined guard that has become REACHED must be removed.
    # Without this the list only ever grows, which is the 0023 shape: a control
    # reporting "closed" about its own contents rather than about the property.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % (_step("python tools/gate/check_a.py")
                                       + _step("python tools/gate/parity_filter.py"))},
              {"check_a.py": stub, "parity_filter.py": stub},
              baseline=_baseline(parity_filter__py=None))
        rc, lines = check(d)
        case("ratchet-healed-entry-must-be-removed",
             rc != 0 and "parity_filter.py" in "\n".join(lines),
             "\n".join(lines)[:200])

    # 10 -- RATCHET: a baselined guard that no longer EXISTS must be removed.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub},
              baseline=_baseline(deleted_guard__py=None))
        rc, lines = check(d)
        case("ratchet-deleted-entry-must-be-removed",
             rc != 0 and "deleted_guard.py" in "\n".join(lines),
             "\n".join(lines)[:200])

    # 11 -- a `kind` outside the taxonomy is an ERROR, so a live gap cannot be
    # parked under an invented label that nothing checks.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub, "parity_filter.py": stub},
              baseline=_baseline(parity_filter__py={
                  "kind": "wontfix",
                  "reason": "a plausible-looking sentence of ample length"}))
        rc, lines = check(d)
        case("baseline-kind-outside-taxonomy-is-an-error",
             rc != 0 and "taxonomy" in "\n".join(lines), "\n".join(lines)[:200])

    # 12 -- an admitted-gap with no issue number is an ERROR. The baseline's
    # own _rules say an admitted gap is not resolved by being listed; without
    # an issue it is simply forgotten.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub, "parity_filter.py": stub},
              baseline=_baseline(parity_filter__py={
                  "kind": "admitted-gap",
                  "reason": "a real orphan, described at respectable length"}))
        rc, lines = check(d)
        case("admitted-gap-without-issue-is-an-error",
             rc != 0 and "issue" in "\n".join(lines), "\n".join(lines)[:200])

    # 12b -- a reached-via-ctest entry with no `add_test` is an ERROR. The
    # kind's whole justification is that the claim is machine-checked; without
    # a target named there is nothing to check and it degrades into an
    # unverifiable amnesty.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub, "parity_filter.py": stub},
              baseline=_baseline(parity_filter__py={
                  "kind": "reached-via-ctest",
                  "reason": "reached from ctest, or so this entry claims"}))
        rc, lines = check(d)
        case("reached-via-ctest-without-target-is-an-error",
             rc != 0 and "add_test" in "\n".join(lines),
             "\n".join(lines)[:200])

    # 12c -- and a named target that NO CMakeLists declares is an ERROR too.
    # This is the case that keeps the kind honest over time: the entry can be
    # perfectly well-formed and still false, because the add_test was renamed
    # or deleted after it was written.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub, "parity_filter.py": stub},
              baseline=_baseline(parity_filter__py={
                  "kind": "reached-via-ctest",
                  "add_test": "a_target_no_cmakelists_declares",
                  "reason": "reached from a ctest target that does not exist"}))
        rc, lines = check(d)
        case("reached-via-ctest-with-absent-target-is-an-error",
             rc != 0 and "declares that target" in "\n".join(lines),
             "\n".join(lines)[:200])

    # 12d -- the green direction. A target that IS declared passes, so the two
    # checks above reject the defect rather than the kind.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub, "parity_filter.py": stub},
              baseline=_baseline(parity_filter__py={
                  "kind": "reached-via-ctest",
                  "add_test": "parity_filter_ctest",
                  "reason": "reached from a ctest target that really exists"}))
        with io.open(os.path.join(d, "CMakeLists.txt"), "w",
                     encoding="utf-8") as f:
            # Split across lines on purpose: CMake allows it, so the checker
            # must normalise whitespace rather than match a formatted string.
            f.write("add_test(\n    NAME parity_filter_ctest\n"
                    "    COMMAND python x.py)\n")
        rc, lines = check(d)
        case("reached-via-ctest-with-declared-target-passes",
             rc == 0, "\n".join(lines)[:200])

    # 13 -- and the well-formed case still PASSES, so the four checks above are
    # rejecting the defect rather than rejecting everything.
    with tempfile.TemporaryDirectory() as d:
        _tree(d, {"a.yml": _WF_AUTO % _step("python tools/gate/check_a.py")},
              {"check_a.py": stub, "parity_filter.py": stub},
              baseline=_baseline(parity_filter__py={
                  "kind": "admitted-gap", "issue": 256,
                  "reason": "a real orphan, described at respectable length"}))
        rc, lines = check(d)
        case("well-formed-baseline-still-passes", rc == 0, "\n".join(lines)[:200])

    width = max(len(r[0]) for r in results)
    for label, ok, detail in results:
        print("  %-*s  %s%s" % (width, label, "PASS" if ok else "FAIL",
                                "" if ok else "   <- %s" % (detail,)))
    bad = [r[0] for r in results if not r[1]]
    if bad:
        print("[guard-reachability] self-test FAILED: %s" % ", ".join(bad))
        return 1
    print("[guard-reachability] self-test OK: %d cases. A dispatch-only wiring, "
          "a shell-comment mention, a docstring mention and a literally-false "
          "step all read as UNREACHED; both indirect routes read as reached; a "
          "guard without the check_ prefix is in scope; and an unparseable "
          "workflow is an error rather than a pass." % len(results))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Guard reachability control (#255)")
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, lines = check(args.repo_root)
    for line in lines:
        print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
