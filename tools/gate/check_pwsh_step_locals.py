#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #280 — flag a workflow ``run:`` block that READS a PowerShell
variable it never ASSIGNS in that same block.

THE DEFECT THIS EXISTS FOR
--------------------------
Every GitHub Actions ``run:`` block is a SEPARATE shell process. A variable
assigned in one step is gone in the next. ``windows-validation.yml``'s
param-id reconciliation step read ``$outDir`` and ``$testExe``, both of which
were assigned only inside the *gtest* step's own block. They arrived as
``$null``, ``Join-Path $null`` threw a mandatory-parameter binding error, and
the step died on its FIRST line on every run it ever made.

That step is deliberately conditioned on gtest having RUN rather than
SUCCEEDED, so it could adjudicate a red run. It never adjudicated anything.
Its two reconciliation layers — built to catch PDF-79, where seventeen
parameterised cases did not exist and the suite reported green for months —
had never once executed. The failure named neither the variable nor the cause,
which is how it survived behind a green-looking suite.

WHY THE EXCLUSION SET IS DERIVED AND NOT TYPED
----------------------------------------------
The tempting shape is a hand-written list of "variables that are fine".
That is precisely the C-ENUM defect (completeness asserted from a hand-made
enumeration) that GitHub #275 and #254 were spent eliminating, and it fails
the same way: whatever the author forgot is a false positive forever, and
whatever they never thought of is invisible.

So the bulk of the exclusion set is asked of the shell itself:

    pwsh -NoProfile -Command "Get-Variable | Select-Object -ExpandProperty Name"

which answers with the automatic variables that actually exist in this
PowerShell edition — 53 of them here — rather than the ones I remembered.

A SHORT RESIDUE IS UNAVOIDABLE, AND IS JUSTIFIED PER ENTRY. ``Get-Variable``
runs in a fresh global scope, so variables that only exist inside a pipeline,
a loop, a regex match or a native-command call are *correctly* absent from its
answer and must be added back. Each one below states why it cannot appear
there. That is a different thing from a preference list: every entry is
derivable-in-principle and explained, and the self-test pins the reason.

SCOPE LIMITS, STATED RATHER THAN DISCOVERED LATER
-------------------------------------------------
* Only ``pwsh``/``powershell`` steps are examined; ``bash``/``cmd`` steps are
  skipped, because this is a PowerShell scoping rule.
* Single-quoted strings and single-quoted here-strings are stripped before
  scanning, because PowerShell does not interpolate them, so a ``$name``
  inside one is not a read. Double-quoted forms ARE scanned — they do
  interpolate.
* ``${{ ... }}`` is stripped first: that is a GitHub expression substituted
  before any shell sees it, not a PowerShell variable.
* This is lexical, not a PowerShell parser. A variable assigned through
  indirection (``Set-Variable -Name (…)``, ``New-Variable``) is not seen as an
  assignment and would be reported. No such site exists in this repo today;
  if one appears, adjudicate it in the baseline with that reason.

The baseline is a TWO-WAY ratchet: a new unadjudicated finding fails, and an
adjudicated entry that has stopped being a finding also fails, so the file
cannot quietly drift out of step with the tree.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Dict, List, Sequence, Set, Tuple

try:
    import yaml
except ImportError:  # pragma: no cover - PyYAML is a hard requirement
    sys.stderr.write("check_pwsh_step_locals: PyYAML is required but missing\n")
    raise SystemExit(2)


# tools/gate/check_pwsh_step_locals.py -> tools/gate -> tools -> <repo root>.
# THREE levels. An earlier sibling guard shipped with two, which made its
# default paths resolve under tools/ and its real mode exit 2 while its
# --self-test passed throughout, because the self-test builds its own trees
# and never exercises how the tool locates inputs it was not given.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DEFAULT_WORKFLOW_DIR = os.path.join(REPO, ".github", "workflows")
DEFAULT_BASELINE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "pwsh_step_locals_baseline.json"
)

PWSH_SHELLS = ("pwsh", "powershell")

#: Automatic variables that ``Get-Variable`` in a fresh global scope cannot
#: report, with the reason each is absent. These are added back to the derived
#: set. The reason is the point: an entry without one is a preference, not a
#: derivation.
SCOPE_CONDITIONAL: Dict[str, str] = {
    "_": "pipeline-scoped; exists only inside a pipeline/scriptblock iteration",
    "PSItem": "alias of $_; same pipeline scope",
    "LASTEXITCODE": "created only after the first native-command invocation",
    "matches": "created only by a successful -match/-cmatch operation",
    "Matches": "case variant of $matches, same origin",
    "foreach": "the loop enumerator, exists only inside a foreach block",
    "switch": "the switch enumerator, exists only inside a switch block",
    "this": "bound only inside a class method or scriptblock with a $this",
    "using": "scope qualifier ($using:x), not a variable of its own",
    "env": "scope qualifier ($env:NAME); the variable token is the scope name",
    "global": "scope qualifier ($global:NAME)",
    "script": "scope qualifier ($script:NAME)",
    "local": "scope qualifier ($local:NAME)",
    "private": "scope qualifier ($private:NAME)",
}


def validate_scope_conditional(automatics: "set") -> "List[str]":
    """GitHub #293 — the residue is an EXCLUSION list, so validate it.

    ``SCOPE_CONDITIONAL`` is unioned into ``excluded``, so every entry SILENCES
    a finding. Until this existed the only thing standing between a bogus entry
    and a permanently-suppressed check was a reason-string length test
    (``len(v) > 15``) in the self-test, which never looked at the KEY at all and
    which sixteen characters of junk would pass. That is what #293 means by the
    residue being "effectively unguarded".

    The check that follows from the table's own stated purpose -- "automatic
    variables that ``Get-Variable`` in a fresh global scope CANNOT report" -- is
    to ask the shell. An entry whose name IS derivable does not need a
    hand-typed exemption, and its presence means the residue is doing work the
    derivation already does, which is exactly how a wrong entry hides.

    The file already asserted this for two names inline
    (``"LASTEXITCODE" not in automatics and "_" not in automatics``) in the
    self-test only. This generalises it to all of them and moves it onto the
    main path, where nothing validated before.

    WHAT THIS DOES NOT CATCH, stated rather than implied
    ---------------------------------------------------
    A name that is neither derivable NOR a real PowerShell automatic variable
    -- ``ZZZ_NOT_A_VARIABLE`` -- passes, because absence from the derived set is
    precisely what the table asserts about its own entries. Closing that needs
    PowerShell's documented automatic-variable list, which is not derivable
    offline and would reintroduce the hand-made enumeration this guard exists to
    replace. So the residue is CHECKED, not PROVEN: a wrong entry that is also
    unreportable still gets in, and the reason string is the only defence left
    for that case.

    Returns a list of problem strings; empty means the table is consistent.
    """
    problems = []

    redundant = sorted(set(SCOPE_CONDITIONAL) & set(automatics))
    for name in redundant:
        problems.append(
            "SCOPE_CONDITIONAL['%s'] is REPORTABLE by Get-Variable, so it is "
            "not scope-conditional at all -- the derived set already covers it. "
            "An entry here silences a finding, so a derivable name in this "
            "table is either redundant or was added to suppress something. "
            "Remove it and let the derivation carry it." % name)

    for name, reason in sorted(SCOPE_CONDITIONAL.items()):
        if not isinstance(reason, str) or not reason.strip():
            problems.append(
                "SCOPE_CONDITIONAL['%s'] has no reason. The reason is the "
                "point: an entry without one is a preference, not a "
                "derivation." % name)

    return problems

# ---------------------------------------------------------------------------
# lexical helpers
# ---------------------------------------------------------------------------

_GH_EXPR = re.compile(r"\$\{\{.*?\}\}", re.DOTALL)
_SQ_HERESTRING = re.compile(r"@'.*?'@", re.DOTALL)
_SQ_STRING = re.compile(r"'[^'\n]*'")
_LINE_COMMENT = re.compile(r"(?m)#.*$")

#: ``$name`` or ``${name}``. The trailing ``(?!\s*:)`` is NOT used — a scope
#: qualifier like ``$env:PATH`` yields the token ``env``, which is excluded by
#: name in SCOPE_CONDITIONAL. Handling it that way keeps ``$global:foo`` and
#: any future qualifier working without a second rule.
_VAR_READ = re.compile(r"\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?")

_ASSIGN_SIMPLE = re.compile(
    r"\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?\s*(?:=|\+=|-=|\*=|/=|%=|\?\?=)(?!=)"
)
_ASSIGN_FOREACH = re.compile(
    r"foreach\s*\(\s*\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?\s+in\b", re.IGNORECASE
)
_ASSIGN_FOR_INIT = re.compile(
    r"for\s*\(\s*\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?\s*=", re.IGNORECASE
)
_ASSIGN_PARAM = re.compile(r"param\s*\((.*?)\)", re.IGNORECASE | re.DOTALL)
#: ``function Name([string]$a, [int]$b) { … }`` — an inline parameter list,
#: which is NOT spelled ``param(...)``. Both real-mode false positives on this
#: guard's first run were this form (``function Get-ArtifactVerdict(
#: [string]$binPath, [string]$artifactPath)`` in windows-fuzz.yml), so it is
#: not a hypothetical. Note the deliberate imprecision: a function parameter is
#: scoped to the function body, so a read of it OUTSIDE the function would be a
#: genuine finding that this rule suppresses. Accepted — the analysis is
#: block-level, not scope-aware, and over-reporting correct code is the failure
#: mode that gets a guard switched off.
_ASSIGN_FUNCPARAM = re.compile(
    r"function\s+[A-Za-z_][\w\-]*\s*\((.*?)\)", re.IGNORECASE | re.DOTALL
)
#: ``-OutVariable out`` / ``-ErrorVariable err`` bind a name WITHOUT a $ sigil.
#: ``Tee-Object -Variable x`` and ``Set-Variable -Name x`` do too, and the bare
#: ``-Variable``/``-Name`` spellings were the third false positive.
_ASSIGN_OUTVAR = re.compile(
    r"-(?:(?:Out|Error|Warning|Information|Debug|Verbose|Pipeline)Variable|Variable|Name)"
    r"\s+\+?\$?\{?([A-Za-z_][A-Za-z0-9_]*)\}?",
    re.IGNORECASE,
)
#: ``$a, $b = 1, 2`` — the simple rule only catches the last name before ``=``.
_ASSIGN_MULTI = re.compile(
    r"(?m)^\s*((?:\$\{?[A-Za-z_][A-Za-z0-9_]*\}?\s*,\s*)+"
    r"\$\{?[A-Za-z_][A-Za-z0-9_]*\}?)\s*="
)


def strip_noninterpolating(code: str) -> str:
    """Remove text in which a ``$name`` is not a variable read.

    Order matters: GitHub expressions first (they may legally contain quotes),
    then single-quoted here-strings (they may contain lone quotes), then
    ordinary single-quoted strings, then line comments.
    """
    code = _GH_EXPR.sub(" ", code)
    code = _SQ_HERESTRING.sub(" ", code)
    code = _SQ_STRING.sub(" ", code)
    code = _LINE_COMMENT.sub(" ", code)
    return code


def assigned_names(code: str) -> Set[str]:
    """Every variable name this block binds, by any form we recognise."""
    names: Set[str] = set()
    for rx in (_ASSIGN_SIMPLE, _ASSIGN_FOREACH, _ASSIGN_FOR_INIT):
        names.update(m.group(1) for m in rx.finditer(code))
    names.update(m.group(1) for m in _ASSIGN_OUTVAR.finditer(code))
    for m in _ASSIGN_MULTI.finditer(code):
        names.update(_VAR_READ.findall(m.group(1)))
    for rx in (_ASSIGN_PARAM, _ASSIGN_FUNCPARAM):
        for m in rx.finditer(code):
            names.update(_VAR_READ.findall(m.group(1)))
    return names


def read_names(code: str) -> Set[str]:
    return set(_VAR_READ.findall(code))


# ---------------------------------------------------------------------------
# the derived exclusion set
# ---------------------------------------------------------------------------

def derive_automatics() -> Tuple[Set[str], str]:
    """Ask PowerShell which automatic variables exist.

    Returns (names, provenance). Raises RuntimeError if the shell cannot be
    consulted — the caller decides whether that is fatal, because a guard that
    silently falls back to a typed list has become the thing it was built to
    replace.
    """
    exe = shutil.which("pwsh") or shutil.which("powershell")
    if not exe:
        raise RuntimeError("neither pwsh nor powershell is on PATH")
    proc = subprocess.run(
        [exe, "-NoProfile", "-NonInteractive", "-Command",
         "(Get-Variable | Select-Object -ExpandProperty Name) -join ','"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "%s exited %d: %s"
            % (os.path.basename(exe), proc.returncode,
               proc.stderr.decode("utf-8", "replace").strip()[:300])
        )
    raw = proc.stdout.decode("utf-8", "replace").strip()
    names = {n.strip() for n in raw.split(",") if n.strip()}
    # Vacuity guard. An empty or near-empty answer means the probe did not
    # really run, and would make EVERY read look excluded-or-unassigned
    # depending on direction. Key on a population the probe PRODUCES.
    if len(names) < 20:
        raise RuntimeError(
            "Get-Variable returned only %d name(s) (%r) — the probe did not "
            "really run; refusing to derive an exclusion set from it"
            % (len(names), sorted(names)[:8])
        )
    return names, "%s Get-Variable (%d names)" % (os.path.basename(exe), len(names))


# ---------------------------------------------------------------------------
# workflow walking
# ---------------------------------------------------------------------------

def rel_to_repo(path: str) -> str:
    """Repo-relative when the path IS under the repo, basename otherwise.

    A bare ``os.path.relpath`` against the repo root turns an out-of-tree path
    (``--workflow-dir`` pointing at a temp dir, as the mutation probe does)
    into a CLIMBING relative path that embeds the build account name:
    ``../../Users/<account>/AppData/Local/Temp/...``. This repo rewrote 1,290
    commits to purge that string and has leaked it twice since, most recently
    through pytest's climbing relative paths. Printed output reaches CI logs,
    which are retained artifacts, so the same rule applies here.
    """
    ap = os.path.abspath(path)
    repo = os.path.abspath(REPO) + os.sep
    if ap.startswith(repo):
        return os.path.relpath(ap, REPO).replace("\\", "/")
    return os.path.basename(ap)


def _shell_for(step: dict, job: dict, wf: dict) -> str:
    for scope in (step, job, wf):
        if not isinstance(scope, dict):
            continue
        sh = scope.get("shell")
        if sh:
            return str(sh)
        defaults = scope.get("defaults")
        if isinstance(defaults, dict):
            run = defaults.get("run")
            if isinstance(run, dict) and run.get("shell"):
                return str(run["shell"])
    return ""


def scan_workflow(path: str) -> Tuple[List[dict], int]:
    """Return (steps, pwsh_step_count) for one workflow file."""
    with io.open(path, "r", encoding="utf-8") as fh:
        try:
            wf = yaml.safe_load(fh.read())
        except yaml.YAMLError as exc:
            raise RuntimeError("%s is not parseable YAML: %s" % (path, exc))
    if not isinstance(wf, dict):
        return [], 0

    out: List[dict] = []
    pwsh_steps = 0
    jobs = wf.get("jobs") or {}
    if not isinstance(jobs, dict):
        return [], 0
    for job_name, job in jobs.items():
        if not isinstance(job, dict):
            continue
        for idx, step in enumerate(job.get("steps") or []):
            if not isinstance(step, dict):
                continue
            run = step.get("run")
            if not isinstance(run, str) or not run.strip():
                continue
            shell = _shell_for(step, job, wf).lower()
            if not any(shell.startswith(s) for s in PWSH_SHELLS):
                continue
            pwsh_steps += 1
            out.append({
                "file": rel_to_repo(path),
                "job": str(job_name),
                "index": idx,
                "name": str(step.get("name") or "(unnamed step #%d)" % idx),
                "run": run,
            })
    return out, pwsh_steps


def findings_for(steps: Sequence[dict], excluded: Set[str]) -> List[dict]:
    found: List[dict] = []
    for step in steps:
        code = strip_noninterpolating(step["run"])
        assigned = assigned_names(code)
        for name in sorted(read_names(code)):
            if name in excluded or name in assigned:
                continue
            found.append({
                "key": "%s|%s|%s|%s" % (step["file"], step["job"], step["name"], name),
                "file": step["file"],
                "job": step["job"],
                "step": step["name"],
                "variable": name,
            })
    return found


# ---------------------------------------------------------------------------
# baseline
# ---------------------------------------------------------------------------

def load_baseline(path: str) -> Dict[str, str]:
    if not os.path.isfile(path):
        return {}
    with io.open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    entries = data.get("adjudicated") or {}
    return {k: str(v) for k, v in entries.items() if not k.startswith("_")}


def run_real(workflow_dir: str, baseline_path: str) -> int:
    if not os.path.isdir(workflow_dir):
        sys.stderr.write("workflow dir not found: %s\n" % workflow_dir)
        return 2

    try:
        automatics, provenance = derive_automatics()
    except RuntimeError as exc:
        # Fail CLOSED and LOUDLY. Substituting a typed list here would turn
        # this guard into the hand-made enumeration it exists to replace, and
        # it would do so silently.
        sys.stderr.write(
            "check_pwsh_step_locals: cannot derive the automatic-variable set "
            "from the shell (%s).\nRefusing to fall back to a hand-written "
            "list — that is the defect this guard replaces.\n" % exc
        )
        return 2

    # GitHub #293. Validate the residue BEFORE trusting it, because the next
    # line unions it into the exclusion set and every entry silences a finding.
    residue_problems = validate_scope_conditional(automatics)
    if residue_problems:
        sys.stderr.write(
            "FAIL: the SCOPE_CONDITIONAL residue is inconsistent with the "
            "derived automatic-variable set (GitHub #293).\n")
        for p in residue_problems:
            sys.stderr.write("  - %s\n" % p)
        return 1

    excluded = set(automatics) | set(SCOPE_CONDITIONAL)

    names = sorted(
        f for f in os.listdir(workflow_dir)
        if f.endswith((".yml", ".yaml"))
    )
    all_steps: List[dict] = []
    total_pwsh = 0
    for n in names:
        steps, cnt = scan_workflow(os.path.join(workflow_dir, n))
        all_steps.extend(steps)
        total_pwsh += cnt

    # Vacuity guard: a scan that reaches nothing passes while looking
    # protective. Key on what the sweep PRODUCED, not on the absence of hits.
    if not names:
        sys.stderr.write("FAIL: no workflow files found under %s\n" % workflow_dir)
        return 1
    if total_pwsh == 0:
        sys.stderr.write(
            "FAIL: scanned %d workflow file(s) and found ZERO pwsh run: steps. "
            "This repo's Windows lanes are PowerShell; zero means the shell "
            "detection broke, not that the tree is clean.\n" % len(names)
        )
        return 1

    found = findings_for(all_steps, excluded)
    found_keys = {f["key"] for f in found}
    adjudicated = load_baseline(baseline_path)
    adj_keys = set(adjudicated)

    unadjudicated = sorted(found_keys - adj_keys)
    stale = sorted(adj_keys - found_keys)

    print("check_pwsh_step_locals")
    print("  exclusion set : %s + %d scope-conditional"
          % (provenance, len(SCOPE_CONDITIONAL)))
    print("  workflows     : %d file(s), %d pwsh run: step(s)"
          % (len(names), total_pwsh))
    print("  findings      : %d (%d adjudicated, %d unadjudicated, %d stale)"
          % (len(found_keys), len(found_keys & adj_keys),
             len(unadjudicated), len(stale)))

    if unadjudicated:
        print("")
        print("FAIL: %d run: block(s) read a PowerShell variable they never assign."
              % len(unadjudicated))
        print("Each GitHub Actions run: block is a SEPARATE process — a variable")
        print("assigned in an earlier step is NOT visible. Derive it in the block,")
        print("or adjudicate it in %s with a reason."
              % rel_to_repo(baseline_path))
        by_key = {f["key"]: f for f in found}
        for k in unadjudicated:
            f = by_key[k]
            print("  %s [%s] step %r reads $%s"
                  % (f["file"], f["job"], f["step"], f["variable"]))
    if stale:
        print("")
        print("FAIL: %d adjudicated entry/entries no longer appear. Remove them —"
              % len(stale))
        print("a baseline that outlives its findings stops describing the tree.")
        for k in stale:
            print("  %s  (reason on file: %s)" % (k, adjudicated[k]))

    return 1 if (unadjudicated or stale) else 0


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

_WF_BAD = """\
name: bad
on: workflow_dispatch
jobs:
  j:
    runs-on: windows-latest
    steps:
      - name: assigns it
        shell: pwsh
        run: |
          $outDir = "$env:GITHUB_WORKSPACE\\out"
          Write-Host $outDir
      - name: reads it without assigning
        shell: pwsh
        run: |
          $listLog = Join-Path $outDir "gtest_list.log"
          Write-Host $listLog
"""

_WF_GOOD = """\
name: good
on: workflow_dispatch
jobs:
  j:
    runs-on: windows-latest
    steps:
      - name: derives in block
        shell: pwsh
        run: |
          $outDir = "$env:GITHUB_WORKSPACE\\out"
          $listLog = Join-Path $outDir "gtest_list.log"
          Write-Host $listLog
"""

_WF_FORMS = """\
name: forms
on: workflow_dispatch
jobs:
  j:
    runs-on: windows-latest
    defaults:
      run:
        shell: pwsh
    steps:
      - name: every binding form
        run: |
          $a = 1
          $b, $c = 2, 3
          foreach ($p in @(1,2)) { Write-Host $p }
          for ($i = 0; $i -lt 2; $i++) { Write-Host $i }
          Get-ChildItem -OutVariable items | Out-Null
          Write-Host $a $b $c $items
          Write-Host $env:PATH
          Write-Host $LASTEXITCODE
          # Both real-mode false positives from this guard's first run, pinned
          # here so the fix cannot silently regress. windows-fuzz.yml had an
          # inline function parameter list and a Tee-Object -Variable bind.
          function Get-Verdict([string]$binPath, [string]$artifactPath) {
            Write-Host $binPath $artifactPath
          }
          Get-ChildItem | Tee-Object -Variable campaignOut | Out-Null
          foreach ($line in $campaignOut) { Write-Host $line }
          'literal $notAVariable here' | Out-Null
          Write-Host "interpolated $a"
      - name: bash is not scanned
        shell: bash
        run: |
          echo "$undeclared_in_bash"
"""


def _write(d: str, name: str, body: str) -> str:
    p = os.path.join(d, name)
    with io.open(p, "w", encoding="utf-8") as fh:
        fh.write(body)
    return p


def self_test() -> int:
    failures: List[str] = []

    def check(label: str, cond: bool, detail: str = "") -> None:
        if cond:
            print("  ok   %s" % label)
        else:
            print("  FAIL %s%s" % (label, (" -- " + detail) if detail else ""))
            failures.append(label)

    print("check_pwsh_step_locals --self-test")

    # 1. the exclusion set really is derived, and the vacuity guard is real
    try:
        automatics, provenance = derive_automatics()
        check("derive_automatics() returns a plausible set", len(automatics) >= 20,
              "got %d" % len(automatics))
        check("derived set contains known automatics",
              {"true", "false", "null", "PWD"} <= automatics,
              "missing: %s" % sorted({"true", "false", "null", "PWD"} - automatics))
        # GitHub #293. This used to name two entries inline -- "LASTEXITCODE"
        # and "_" -- so twelve of the fourteen went unchecked. Generalised.
        _derivable = sorted(set(SCOPE_CONDITIONAL) & set(automatics))
        check("derived set OMITS every scope-conditional name (so each entry is needed)",
              not _derivable,
              "these are derivable and should not be hand-listed: %s" % _derivable)
    except RuntimeError as exc:
        check("derive_automatics() works on this host", False, str(exc))
        automatics = set()

    check("every scope-conditional entry carries a reason",
          all(isinstance(v, str) and v.strip() and len(v) > 15
              for v in SCOPE_CONDITIONAL.values()))

    # GitHub #293 -- the residue validator must REDDEN, not merely pass.
    # Without these two the generalisation above proves only that today's table
    # happens to be clean, which is a property of the data rather than of the
    # check. An all-green self-test cannot tell a working guard from a guard
    # that has stopped being able to say no.
    if automatics:
        _live = dict(SCOPE_CONDITIONAL)
        try:
            _derivable_name = sorted(automatics)[0]
            SCOPE_CONDITIONAL[_derivable_name] = "injected by the self-test"
            check("a DERIVABLE name in the residue is rejected",
                  any("REPORTABLE" in p
                      for p in validate_scope_conditional(automatics)),
                  "injected %r, which derive_automatics() reports, and the "
                  "validator did not object" % _derivable_name)
        finally:
            SCOPE_CONDITIONAL.clear()
            SCOPE_CONDITIONAL.update(_live)

        _live = dict(SCOPE_CONDITIONAL)
        try:
            SCOPE_CONDITIONAL["ZZZ_SELFTEST_NO_REASON"] = "   "
            check("an entry with a blank reason is rejected",
                  any("no reason" in p
                      for p in validate_scope_conditional(automatics)))
        finally:
            SCOPE_CONDITIONAL.clear()
            SCOPE_CONDITIONAL.update(_live)

        check("the LIVE residue is accepted unchanged (specificity)",
              not validate_scope_conditional(automatics),
              "the committed table does not satisfy its own validator: %s"
              % validate_scope_conditional(automatics))

    excluded = set(automatics) | set(SCOPE_CONDITIONAL)

    # 2. the real defect shape is detected, and the fixed shape is not
    tmp = tempfile.mkdtemp(prefix="pwsh_locals_selftest_")
    try:
        bad = _write(tmp, "bad.yml", _WF_BAD)
        good = _write(tmp, "good.yml", _WF_GOOD)
        forms = _write(tmp, "forms.yml", _WF_FORMS)

        bad_steps, bad_cnt = scan_workflow(bad)
        bad_found = findings_for(bad_steps, excluded)
        check("the #280 shape is DETECTED",
              any(f["variable"] == "outDir" for f in bad_found),
              "found %r" % [f["variable"] for f in bad_found])
        check("only the offending step is flagged, not the assigning one",
              all(f["step"] == "reads it without assigning" for f in bad_found),
              "found %r" % [(f["step"], f["variable"]) for f in bad_found])

        good_steps, _ = scan_workflow(good)
        good_found = findings_for(good_steps, excluded)
        check("the FIXED shape is clean (guard does not only-redden)",
              good_found == [], "found %r" % [f["variable"] for f in good_found])

        forms_steps, forms_cnt = scan_workflow(forms)
        forms_found = findings_for(forms_steps, excluded)
        check("all binding forms recognised (=, multi, foreach, for, -OutVariable)",
              forms_found == [], "found %r" % [f["variable"] for f in forms_found])
        check("job-level defaults.run.shell is honoured", forms_cnt == 1,
              "counted %d pwsh step(s)" % forms_cnt)
        check("bash steps are skipped", bad_cnt == 2 and forms_cnt == 1)

        # 3. non-interpolating text really is ignored
        check("single-quoted text is not a read",
              "notAVariable" not in read_names(strip_noninterpolating(
                  "'literal $notAVariable here'")))
        check("double-quoted text IS a read",
              "yes" in read_names(strip_noninterpolating('"got $yes"')))
        check("${{ }} expressions are not reads",
              read_names(strip_noninterpolating("${{ inputs.ui_only }}")) == set())
        check("single-quoted here-strings are not reads",
              "inner" not in read_names(strip_noninterpolating("@'\n$inner\n'@")))
        check("scope qualifiers reduce to the scope token",
              read_names("$env:PATH") == {"env"})

        # 4. baseline ratchet, both directions
        bl = os.path.join(tmp, "baseline.json")
        key = bad_found[0]["key"] if bad_found else "x"
        with io.open(bl, "w", encoding="utf-8") as fh:
            json.dump({"adjudicated": {key: "self-test"}}, fh)
        check("an adjudicated finding is accepted", load_baseline(bl) == {key: "self-test"})
        with io.open(bl, "w", encoding="utf-8") as fh:
            json.dump({"adjudicated": {"gone|j|step|nope": "stale"}}, fh)
        stale_keys = set(load_baseline(bl)) - {f["key"] for f in bad_found}
        check("a stale adjudicated entry is visible to the ratchet",
              stale_keys == {"gone|j|step|nope"})

        # 5. the locator bug that a self-test cannot catch by construction
        check("REPO resolves to the repo root, not tools/",
              os.path.isdir(os.path.join(REPO, ".github", "workflows")),
              "REPO=%s" % REPO)

        # 6. path hygiene of the OUTPUT. The mutation probe runs with
        # --workflow-dir under TEMP, and a bare relpath turned that into
        # ../../Users/<account>/... in printed output. CI logs are retained
        # artifacts, so that is a leak channel.
        outside = rel_to_repo(os.path.join(tmp, "bad.yml"))
        check("an out-of-repo path does not print a climbing relative path",
              not outside.startswith("..") and "/" not in outside,
              "got %r" % outside)
        check("an out-of-repo path does not print the user profile dir",
              "Users" not in outside and "AppData" not in outside,
              "got %r" % outside)
        check("an in-repo path still prints repo-relative",
              rel_to_repo(os.path.join(REPO, ".github", "workflows", "fast-checks.yml"))
              == ".github/workflows/fast-checks.yml")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    if failures:
        print("SELF-TEST FAILED: %d case(s): %s" % (len(failures), ", ".join(failures)))
        return 1
    print("SELF-TEST OK")
    return 0


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--workflow-dir", default=DEFAULT_WORKFLOW_DIR)
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(list(argv))
    if args.self_test:
        return self_test()
    return run_real(args.workflow_dir, args.baseline)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
