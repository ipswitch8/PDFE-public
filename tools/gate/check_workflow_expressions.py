#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_workflow_expressions.py — reject workflow expressions GitHub will not accept.

    python tools/gate/check_workflow_expressions.py --self-test
    python tools/gate/check_workflow_expressions.py --repo-root .

WHY THIS EXISTS

2026-10-11: `df57fb7c` put `matrix.shard == 1` in `jobs.ui-e2e.if`. The `matrix`
context is NOT available in `jobs.<id>.if` — only `github`, `needs`, `vars` and
`inputs` are. That did not produce a failing job; it made
`.github/workflows/windows-validation.yml` an **INVALID WORKFLOW FILE**, which
is strictly worse:

  * GitHub emits a failed run named after the FILE PATH, with no jobs, on every
    push (run 38182505254);
  * the workflow becomes UNDISPATCHABLE, so the targeted validation that commit
    existed to enable could not be run at all.

**PyYAML parses what GitHub rejects.** The file was valid YAML, `${{`/`}}` were
balanced, and the local aggregate passed 104 checks — the defect is in
expression semantics, not syntax, and no local check looked at those. The
mistake survived to master and was found only by reading CI state afterwards.

`actionlint` would cover this and more, but it is not installed on this host and
is not on PATH, so this implements the specific rules rather than adding a
toolchain dependency to the only auto-triggered workflow.

THREE RULES, each chosen to be cheap and near-zero false-positive:

  A. POSITION — in ``jobs.<id>.if`` the context roots must be a subset of
     {github, needs, vars, inputs}. This is the rule that was violated.
  B. VOCABULARY — every context root used anywhere must be a real GitHub
     context. Catches `input.x` for `inputs.x`, `matrix` misspellings, and the
     like, which are equally fatal and equally invisible to a YAML parse.
  C. SCOPE — the ``steps`` context must not appear in any job-level key. It only
     exists inside a step.
  D. DEPENDENCY — a step invoking a ``tools/gate`` script must run in a job
     whose ``pip install`` provides that script's third-party imports.

Deliberately NOT attempting the whole context-availability matrix: a guard that
misfires gets disabled, and the remaining positions have far more conditional
nuance than these three.

WHY RULE D EXISTS — this guard's own first CI run broke on it
------------------------------------------------------------
`d8ee05bc` wired the step above into the `installer-dry-run` job, which installs
only pytest. Both invocations died on ``PyYAML is required`` (run 38198830707),
reddening Fast Checks. The guard itself was correct and passed locally; only its
PLACEMENT was wrong.

That failure mode is worth a rule because of how it presents: a missing
dependency exits non-zero and reads in the log exactly like a real finding, so
the natural response is to go looking for a defect in the tree that is not
there. It is the mirror image of the hazard this file already guards — there, a
parse succeeded and hid a semantic error; here, a failure succeeded at looking
like evidence. Both are cases of a signal meaning something other than what it
appears to mean.

Rule D is static and needs no execution: a job's pip line and a script's import
list are both readable from disk. It fails CLOSED on an unrecognised import —
an unknown module is reported, not assumed present — because the whole point is
that silence must not be mistaken for provision.
"""
from __future__ import annotations

import argparse
import ast
import glob
import os
import re
import sys

try:
    import yaml
except ImportError:                                      # pragma: no cover
    print("check_workflow_expressions: PyYAML is required", file=sys.stderr)
    raise SystemExit(2)

# Every context GitHub defines. A root outside this set cannot resolve.
KNOWN_CONTEXTS = frozenset((
    "github", "env", "vars", "job", "jobs", "steps", "runner", "secrets",
    "strategy", "matrix", "needs", "inputs",
))

# Available in jobs.<job_id>.if — per GitHub's context-availability table.
JOB_IF_ALLOWED = frozenset(("github", "needs", "vars", "inputs"))

# Expression functions are not contexts; an identifier followed by '(' is a call.
_EXPR = re.compile(r"\$\{\{(.*?)\}\}", re.S)
_ROOT = re.compile(r"(?<![\w.'\"])([A-Za-z_][A-Za-z0-9_-]*)\s*(\.|\[)")
_CALL = re.compile(r"(?<![\w.])([A-Za-z_][A-Za-z0-9_-]*)\s*\(")


_STRLIT = re.compile(r"'[^']*'|\"[^\"]*\"")


def context_roots(text: str) -> set:
    """Context roots referenced by every ``${{ }}`` block in *text*.

    A bare ``if:`` value in a workflow may omit the ``${{ }}`` wrapper, so the
    whole string is also scanned when it contains no block — otherwise the rule
    could be bypassed by dropping the braces, which GitHub allows.

    STRING LITERALS ARE BLANKED FIRST. Without that,
    ``${{ inputs.x == '' && 'bin/Debug/pdfe_tests.exe' || '' }}`` yields a root
    of `pdfe_tests` from inside the quoted path and gets reported as an unknown
    context. Found by running this guard against the repo's own 14 workflows on
    its first execution — which is why it is run against the real tree and not
    only against planted samples.
    """
    bodies = _EXPR.findall(text)
    if not bodies:
        bodies = [text]
    roots = set()
    for body in bodies:
        body = _STRLIT.sub(lambda m: " " * len(m.group(0)), body)
        calls = set(_CALL.findall(body))
        for m in _ROOT.finditer(body):
            name = m.group(1)
            if name in calls:
                continue
            roots.add(name)
    return roots


def _as_text(value) -> str:
    return value if isinstance(value, str) else ""


# ------------------------------------------------------------------ rule D
# Import name -> the pip distribution that provides it. They differ often
# enough that a naive `import name in pip line` test would be wrong in both
# directions: `yaml` ships as `pyyaml`, `fitz` as `pymupdf`.
IMPORT_TO_PIP = {
    "yaml": "pyyaml",
    "fitz": "pymupdf",
    "pymupdf": "pymupdf",
    "fontTools": "fonttools",
    "PIL": "pillow",
    "pikepdf": "pikepdf",
    "numpy": "numpy",
    "pytest": "pytest",
    "requests": "requests",
    "lxml": "lxml",
    "clang": "libclang",          # `import clang.cindex` ships as libclang
    "cryptography": "cryptography",
}

_PIP_LINE = re.compile(r"pip\d?\s+install\s+([^\n;&|]*)", re.I)
_GATE_CALL = re.compile(r"python\d?\s+(tools/gate/[\w./-]+\.py)")


def job_pip_packages(job: dict) -> set:
    """Distributions a job's own steps install, lower-cased.

    Only the job's steps count. A sibling job's pip line provides nothing
    here — which is exactly the mistake rule D exists to catch.
    """
    found = set()
    for st in (job.get("steps") or []):
        run = _as_text(st.get("run"))
        for m in _PIP_LINE.finditer(run):
            for tok in m.group(1).split():
                if tok.startswith("-") or tok in ("python", "-m", "pip"):
                    continue
                # strip a version pin: `pyyaml==6.0` -> `pyyaml`
                found.add(re.split(r"[=<>!~\[]", tok)[0].strip().lower())
    return found


_LOCAL_CACHE = {}


def _is_local_module(root: str, script_path: str, repo_root: str) -> bool:
    """True if *root* resolves to something inside the repo.

    These scripts reach sibling helpers by appending to ``sys.path`` — e.g.
    ``run_gtest`` from ``tests/scripts``, ``gen_mupdf_recursion_dos``, and the
    ``tests`` package itself. Those are not distributions and must not be
    demanded of a pip line. Reported as unknown imports on rule D's first run
    against the tree.
    """
    gate_dir = os.path.dirname(script_path)
    if os.path.exists(os.path.join(gate_dir, root + ".py")):
        return True
    if repo_root not in _LOCAL_CACHE:
        names = set()
        for dirpath, dirnames, filenames in os.walk(repo_root):
            # skip the expensive, irrelevant trees
            dirnames[:] = [d for d in dirnames
                           if d not in (".git", "build", "vcpkg_installed",
                                        "node_modules", "__pycache__")]
            for f in filenames:
                if f.endswith(".py"):
                    names.add(f[:-3])
            for d in dirnames:
                names.add(d)
        _LOCAL_CACHE[repo_root] = names
    return root in _LOCAL_CACHE[repo_root]


def script_third_party_imports(script_path: str, repo_root: str = "") -> set:
    """MODULE-LEVEL third-party module roots imported by *script_path*.

    Scope matters, and getting it wrong cost a false positive in both
    directions on the first real run:

    * **Module-level only.** A function-local ``import cryptography`` in
      ``check_crl_chain_fixture.py`` is deliberately conditional — it is caught
      and reported as ``unavailable``. Demanding it of the job's pip line
      flagged a script that handles absence correctly. ``ast.walk`` ignores
      scope, so it is wrong here; the top-level body is what matters.
    * **A module-level ``try:`` still counts.** This file's own ``yaml`` import
      sits in a try/except that prints and exits 2. That is a hard dependency
      expressed defensively, not an optional one. Treating it as optional is
      exactly how the dependency went unnoticed and reddened Fast Checks.
    """
    try:
        with open(script_path, encoding="utf-8") as fh:
            tree = ast.parse(fh.read(), filename=script_path)
    except (OSError, SyntaxError):
        return set()

    stdlib = getattr(sys, "stdlib_module_names", frozenset())
    repo_root = repo_root or os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(script_path))))

    # Top-level statements, descending into module-level try/if only.
    def top_level(body):
        for node in body:
            if isinstance(node, (ast.Import, ast.ImportFrom)):
                yield node
            elif isinstance(node, ast.Try):
                for sub in (node.body + node.orelse + node.finalbody):
                    if isinstance(sub, (ast.Import, ast.ImportFrom)):
                        yield sub
                for h in node.handlers:
                    for sub in h.body:
                        if isinstance(sub, (ast.Import, ast.ImportFrom)):
                            yield sub
            elif isinstance(node, ast.If):
                for sub in top_level(node.body + node.orelse):
                    yield sub

    roots = set()
    for node in top_level(tree.body):
        if isinstance(node, ast.Import):
            names = [a.name for a in node.names]
        else:
            if node.level:                      # relative import: local
                continue
            names = [node.module or ""]
        for n in names:
            root = n.split(".")[0]
            if not root or root in stdlib or root == "__future__":
                continue
            if _is_local_module(root, script_path, repo_root):
                continue
            roots.add(root)
    return roots


def check_job_dependencies(path: str, doc, repo_root: str) -> list:
    """Rule D over one workflow document."""
    out = []
    jobs = (doc or {}).get("jobs") or {}
    if not isinstance(jobs, dict):
        return out
    for job_name, job in jobs.items():
        if not isinstance(job, dict):
            continue
        installed = job_pip_packages(job)
        for st in (job.get("steps") or []):
            run = _as_text(st.get("run"))
            for rel in set(_GATE_CALL.findall(run)):
                script = os.path.join(repo_root, rel.replace("/", os.sep))
                if not os.path.exists(script):
                    continue
                for mod in sorted(script_third_party_imports(script, repo_root)):
                    pkg = IMPORT_TO_PIP.get(mod)
                    if pkg is None:
                        out.append((path, job_name, "D",
                                    "`%s` imports `%s`, which this guard does "
                                    "not know how to map to a pip distribution "
                                    "-- add it to IMPORT_TO_PIP rather than "
                                    "assuming it is present" % (rel, mod),
                                    (st.get("name") or rel)[:90]))
                    elif pkg not in installed:
                        out.append((path, job_name, "D",
                                    "`%s` imports `%s` (pip `%s`) but job "
                                    "`%s` never installs it -- the step will "
                                    "die on an ImportError that reads like a "
                                    "real finding"
                                    % (rel, mod, pkg, job_name),
                                    (st.get("name") or rel)[:90]))
    return out


def check_workflow(path: str, doc) -> list:
    """Findings for one parsed workflow document."""
    out = []
    jobs = (doc or {}).get("jobs") or {}
    if not isinstance(jobs, dict):
        return out
    for job_name, job in jobs.items():
        if not isinstance(job, dict):
            continue

        cond = _as_text(job.get("if"))
        if cond:
            roots = context_roots(cond)
            bad = sorted(r for r in roots
                         if r in KNOWN_CONTEXTS and r not in JOB_IF_ALLOWED)
            for r in bad:
                out.append((path, job_name, "A",
                            "`%s` is not available in jobs.<id>.if (allowed: %s)"
                            % (r, ", ".join(sorted(JOB_IF_ALLOWED))),
                            cond.strip()[:120]))

        # Rules B and C over the whole job, including its steps.
        def walk(node, at):
            if isinstance(node, dict):
                for k, v in node.items():
                    walk(v, at + [str(k)])
            elif isinstance(node, list):
                for i, v in enumerate(node):
                    walk(v, at + [str(i)])
            elif isinstance(node, str) and "${{" in node:
                roots = context_roots(node)
                for r in sorted(roots):
                    if r not in KNOWN_CONTEXTS:
                        out.append((path, job_name, "B",
                                    "`%s` is not a GitHub context" % r,
                                    "/".join(at) + ": " + node.strip()[:90]))
                # `steps` is legal inside a step AND inside jobs.<id>.outputs.*,
                # which is evaluated after the steps have run and is the
                # ordinary way a job exports a step outcome. Flagging that was
                # this guard's own first false positive, on four real jobs.
                if ("steps" in roots
                        and "steps" not in at and "outputs" not in at):
                    out.append((path, job_name, "C",
                                "`steps` context used outside a step or "
                                "jobs.<id>.outputs",
                                "/".join(at) + ": " + node.strip()[:90]))

        walk(job, [])
    return out


def scan(repo_root: str):
    pattern = os.path.join(repo_root, ".github", "workflows", "*.y*ml")
    findings, n_files, n_jobs, n_job_ifs, n_gate_calls = [], 0, 0, 0, 0
    for path in sorted(glob.glob(pattern)):
        n_files += 1
        with open(path, encoding="utf-8") as fh:
            try:
                doc = yaml.safe_load(fh)
            except yaml.YAMLError as e:
                findings.append((path, "-", "YAML", "does not parse: %s" % e, ""))
                continue
        jobs = (doc or {}).get("jobs") or {}
        if isinstance(jobs, dict):
            n_jobs += len(jobs)
            n_job_ifs += sum(1 for j in jobs.values()
                             if isinstance(j, dict) and j.get("if"))
            for j in jobs.values():
                if isinstance(j, dict):
                    for st in (j.get("steps") or []):
                        n_gate_calls += len(set(
                            _GATE_CALL.findall(_as_text(st.get("run")))))
        findings.extend(check_workflow(path, doc))
        findings.extend(check_job_dependencies(path, doc, repo_root))
    return findings, n_files, n_jobs, n_job_ifs, n_gate_calls


# --------------------------------------------------------------------- self-test
def self_test() -> int:
    """Planted positives AND negatives.

    A guard no input can redden is decoration; one that reddens on correct input
    gets disabled. Both directions are exercised here, and the POSITIVE case is
    the exact expression that broke windows-validation.yml.
    """
    ok = True

    def case(label, doc, expect_rules):
        nonlocal ok
        got = sorted({f[2] for f in check_workflow("<self-test>", doc)})
        want = sorted(set(expect_rules))
        good = got == want
        ok = ok and good
        print("  [%s] %-56s rules=%s expected=%s"
              % ("ok" if good else "XX", label, got or "[]", want or "[]"))

    # The real defect.
    case("matrix in jobs.<id>.if (the 2026-10-11 defect)",
         {"jobs": {"ui-e2e": {"if": "${{ !cancelled() && (inputs.ui_only == '' "
                                    "|| matrix.shard == 1) }}"}}}, ["A"])
    # The same condition where it is legal.
    case("matrix in a step if (legal)",
         {"jobs": {"ui-e2e": {"steps": [{"if": "${{ matrix.shard == 1 }}"}]}}}, [])
    # What the fixed file looks like.
    case("inputs+needs in jobs.<id>.if (legal)",
         {"jobs": {"m": {"if": "${{ !cancelled() && !inputs.ui_only "
                               "&& needs.build.outputs.built == 'success' }}"}}}, [])
    # Functions must not be mistaken for contexts.
    case("functions only, no contexts (legal)",
         {"jobs": {"r": {"if": "${{ always() }}"}}}, [])
    case("cancelled() + success() (legal)",
         {"jobs": {"r": {"if": "${{ !cancelled() && success() }}"}}}, [])
    # Vocabulary.
    case("typo `input.` for `inputs.`",
         {"jobs": {"a": {"steps": [{"run": "echo ${{ input.foo }}"}]}}}, ["B"])
    case("unknown context root",
         {"jobs": {"a": {"steps": [{"run": "echo ${{ mtrix.shard }}"}]}}}, ["B"])
    # Scope.
    case("steps context at job level",
         {"jobs": {"a": {"env": {"X": "${{ steps.foo.outputs.bar }}"}}}}, ["B", "C"]
         if "steps" not in KNOWN_CONTEXTS else ["C"])
    case("steps context inside a step (legal)",
         {"jobs": {"a": {"steps": [{"run": "echo ${{ steps.x.outputs.y }}"}]}}}, [])
    # A bare if: with no braces must still be checked.
    case("bare (unbraced) if with matrix",
         {"jobs": {"a": {"if": "matrix.shard == 1"}}}, ["A"])

    # --- REGRESSION CASES: both were this guard's OWN false positives on the
    # repo's real workflows, found on its first run against the tree. ---
    case("steps in jobs.<id>.outputs (legal, 4 real jobs do this)",
         {"jobs": {"build": {"outputs": {"built": "${{ steps.build_debug.outcome }}"}}}},
         [])
    case("identifier inside a quoted path is not a context",
         {"jobs": {"core-tests": {"steps": [{"with": {"required-files":
          "bin/Debug/pdfe.exe\n"
          "${{ inputs.ui_only == '' && 'bin/Debug/pdfe_tests.exe' || '' }}"
          }}]}}}, [])

    # --- RULE D, on real files, because it reads imports off disk ---
    import shutil
    import tempfile

    def dcase(label, script_src, pip_line, expect):
        nonlocal ok
        td = tempfile.mkdtemp()
        try:
            gd = os.path.join(td, "tools", "gate")
            os.makedirs(gd)
            with open(os.path.join(gd, "g.py"), "w", encoding="utf-8") as fh:
                fh.write(script_src)
            steps = []
            if pip_line:
                steps.append({"run": pip_line})
            steps.append({"run": "python tools/gate/g.py --repo-root ."})
            doc = {"jobs": {"j": {"steps": steps}}}
            got = sorted({f[2] for f in
                          check_job_dependencies("<self-test>", doc, td)})
            good = got == sorted(set(expect))
            ok = ok and good
            print("  [%s] %-56s rules=%s expected=%s"
                  % ("ok" if good else "XX", label, got or "[]",
                     sorted(set(expect)) or "[]"))
        finally:
            shutil.rmtree(td, ignore_errors=True)

    # THE REAL DEFECT: this very script in a job with no pyyaml (run 38198830707).
    dcase("hard `import yaml`, job installs only pytest",
          "import yaml\n", "python -m pip install pytest", ["D"])
    dcase("hard `import yaml`, job installs pyyaml (legal)",
          "import yaml\n", "python -m pip install pytest pyyaml", [])
    # A try-guarded module-level import is still a hard dependency.
    dcase("try-guarded `import yaml` is still required",
          "try:\n    import yaml\nexcept ImportError:\n    raise SystemExit(2)\n",
          "python -m pip install pytest", ["D"])
    # A FUNCTION-local import is conditional by construction -- the
    # check_crl_chain_fixture.py `cryptography` false positive.
    dcase("function-local import is not demanded (legal)",
          "def f():\n    import cryptography\n    return cryptography\n",
          "python -m pip install pytest", [])
    # Stdlib is never demanded of a pip line.
    dcase("stdlib-only script (legal)",
          "import os, sys, json, re\n", "python -m pip install pytest", [])
    # A version pin must still satisfy the requirement.
    dcase("pinned `pyyaml==6.0.1` satisfies it (legal)",
          "import yaml\n", "python -m pip install pyyaml==6.0.1", [])

    print("\nself-test: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--min-job-ifs", type=int, default=3,
                    help="refuse to pass if fewer job-level if: expressions "
                         "were found than this; a sweep that matches nothing "
                         "passes for free")
    ap.add_argument("--min-gate-calls", type=int, default=10,
                    help="refuse to pass if rule D inspected fewer gate-script "
                         "invocations than this, for the same reason")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    findings, n_files, n_jobs, n_job_ifs, n_gate_calls = scan(args.repo_root)
    print("[workflow-expr] %d workflow(s), %d job(s), %d job-level if(s), "
          "%d gate-script invocation(s) dependency-checked"
          % (n_files, n_jobs, n_job_ifs, n_gate_calls))

    # Non-vacuity for rule D as well: it is worthless if it inspected nothing.
    if n_gate_calls < args.min_gate_calls:
        print("[workflow-expr] FAIL: only %d gate-script invocation(s) found "
              "(need >= %d), so rule D checked almost nothing and its pass "
              "carries no information."
              % (n_gate_calls, args.min_gate_calls), file=sys.stderr)
        return 1

    # Non-vacuity: this guard's whole value is reading expressions, so it must
    # have found some. Zero means it is pointed at the wrong tree.
    if n_files == 0 or n_job_ifs < args.min_job_ifs:
        print("[workflow-expr] FAIL: found %d workflow(s) and %d job-level if(s) "
              "(need >= %d). The scan is not seeing the tree it polices, so a "
              "pass here would be meaningless."
              % (n_files, n_job_ifs, args.min_job_ifs), file=sys.stderr)
        return 1

    if findings:
        print("[workflow-expr] FAIL: %d invalid expression(s). GitHub rejects "
              "these at the FILE level -- the workflow becomes undispatchable "
              "and every push reports a failed run named after the file path."
              % len(findings), file=sys.stderr)
        for path, job, rule, msg, where in findings:
            rel = os.path.relpath(path, args.repo_root).replace("\\", "/")
            print("  %s [job %s] rule %s: %s" % (rel, job, rule, msg),
                  file=sys.stderr)
            if where:
                print("      %s" % where, file=sys.stderr)
        return 1

    print("[workflow-expr] OK: every workflow expression uses a context that is "
          "available where it is used.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
