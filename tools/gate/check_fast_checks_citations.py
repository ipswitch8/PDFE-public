#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Standing control for the step-skip masking defect and its documentation debt (F43).

WHAT THIS CLOSES
================
On CI run ``31242065496`` step 8 of ``fast-checks.yml`` (*DCA loop*) concluded
``failure`` and steps 9-16 all concluded ``skipped``.  GitHub Actions applies an
implicit ``success()`` to every step that carries no ``if:``, so ONE red step
silently disabled EIGHT independent standing controls:

    DCA presence (R-26/R-31), flaky-quarantine (R-37), RTM live-UI citations
    (F29), the WIN-45 reachability-skip ratchet (F29), the ASan cadence bound
    (F30), the PERF-02 magnitude-guard ratchet (F37), the shared-runner queue
    bound (F38), and the gate self-tests (R-24/R-26/R-28/R-32/R-37).

Phase-42 made the DCA step green, so the *symptom* went away.  F43 removes the
*mechanism*: every independent check step now carries ``if: ${{ !cancelled() }}``.
This checker is the ratchet that stops the mechanism coming back.

THE SECOND DEFECT: VOLATILE CITATIONS
=====================================
Inserting twelve ``if:`` lines shifts every line number in ``fast-checks.yml``.
At the time of writing the repository carried 64 ``fast-checks.yml:NNN``
citations across 15 files -- and, once the sweep was widened to ``governance.yml``
as well, **109 citations across 21 files**, six of which no hand-built inventory
had found.  A re-derivation showed some were ALREADY stale
before F43 touched anything -- ``rtm-coverage.md``'s ``:170`` pointer at the
magnitude-guard self-test rotted within a day of F37 writing it, because F38
added a step above it.  A line number into a file that is edited every phase is
not a citation, it is a decaying pointer.

F43's policy (see docs/audit/meta-audit-2026-07/followup/phase-43-dispositions.md):

  * HISTORICAL, dated records keep their line numbers.  A disposition saying
    "as of phase-30 the step was at :92" is a record of a past observation, and
    rewriting it to quiet a sweep falsifies history.  This follows the F32 /
    F39 / F40 precedent: sweep on the REFERENT, update LIVE claims, leave dated
    history as history, and record which is which.
  * LIVE documents cite the step by NAME, never by line.  Step names are stable
    across edits, are what a reader actually needs, and cannot rot silently.

GATES
=====
G1  Every non-setup step in each target workflow carries a non-cancelled guard
    (``!cancelled()`` or ``always()``); setup steps carry NO ``if:`` so the
    environment still fails fast.
G2  Neither target workflow uses ``continue-on-error``.  A guard that ALSO
    swallowed the failure would be far worse than the masking it replaced:
    ``!cancelled()`` must change WHETHER A STEP RUNS, never whether its failure
    counts.  (windows-fuzz.yml / windows-validation.yml document two prior
    false greens from exactly that flag; see R-32 and GAP-A15.)
G3  Every tracked file containing a ``<workflow>.yml:NNN`` citation is
    classified LIVE or HISTORICAL.  A new, unclassified file FAILS, which
    forces the classification decision rather than letting it default.
G4  LIVE documents contain ZERO line-number citations into the targets.
G5  Every step-name anchor quoted in a LIVE document resolves to a real step.
G6  The eight steps that run 31242065496 actually masked still exist by name
    and are guarded -- the incident, pinned, so a rename cannot quietly drop
    one out of G1's scope.

STATED LIMITATION OF G4
=======================
G4 matches ``<basename>.yml:NNN``.  It does NOT match a BARE ``:NNN`` written as
a follow-on reference to a workflow named earlier in the sentence -- which is
exactly the form of the pointer that rotted (``rtm-coverage.md`` said the
self-test was "named in that workflow's *Gate self-tests* step (``:170``)").
A bare ``:NNN`` cannot be distinguished mechanically from a time, a port, or a
citation into some other file, so gating it would be noise.  The mitigation is
the anchor convention itself: a LIVE document that names the step has no reason
to add a line number after it.  Recorded rather than papered over, because the
gate is narrower than the policy it enforces.


RESOLVER
========
``--list-steps`` / ``--resolve NAME`` re-derive CURRENT line numbers from step
names.  That is the mechanical replacement for hand-counted ``:NNN`` citations:
anyone who wants a line number derives it at the moment they need it.

PyYAML NOTE: a bare ``on:`` key parses as the BOOLEAN ``True``, not the string
``"on"``.  Nothing here reads triggers, but the trap is recorded because two
previous checkers in this directory had to learn it.

CRLF NOTE: ``core.autocrlf=true``.  Everything here is line-oriented and reads
text with universal newlines, so no comparison depends on the byte form.

Exit: 0 all gates pass; 1 a gate failed; 2 usage/IO error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# --------------------------------------------------------------------------
# Targets and their SETUP steps.  A setup step deliberately keeps fail-fast
# semantics: if the checkout or the interpreter is missing, the checks below it
# are meaningless and must not run.
# --------------------------------------------------------------------------
TARGETS = {
    ".github/workflows/fast-checks.yml": {
        "Checkout",
        "Set up Python",
        "Install check deps",
    },
    ".github/workflows/governance.yml": {
        "Checkout",
    },
}

# The eight steps run 31242065496 masked, by name.  Pinned so a rename cannot
# silently remove one from G1's scope.
REGRESSION_STEPS = [
    "DCA presence — a defect-fix range must add a DCA record (R-26/R-31)",
    "Flaky-quarantine registry valid + unexpired (R-37)",
    "RTM cites every reachability-converted live-UI file (F29)",
    "No new reachability-skip statement shapes (WIN-45 ratchet, F29)",
    "ASan lane's detection window stays bounded (F30)",
    "No unreviewed magnitude guard in tests (PERF-02 ratchet, F37)",
    "Shared-runner queue exposure stays within the accepted bound (F38)",
    "Gate self-tests + matrix pass-semantics (R-24/R-26/R-28/R-32/R-37)",
]

# --------------------------------------------------------------------------
# Citation classification.  Per-FILE, because per-citation "is this sentence
# dated?" is not mechanically decidable and guessing it is how this programme
# produces claims its evidence cannot establish.
#
# LIVE   -- present-tense claims maintained as the tree changes.
# HIST   -- dated records.  Each carries the reason it is dated; rewriting any
#           of them would falsify an observation made at a stated time.
# --------------------------------------------------------------------------
LIVE_DOCS = {
    "docs/audit/rtm-coverage.md":
        "the requirements-traceability matrix; present-tense 'X is wired at Y' "
        "claims are maintained against the current tree",
}

HISTORICAL_DOCS = {
    "docs/audit/p97-phase9/probes/issue_fast_checks_selftest_comment.md":
        "the body filed as GitHub #311, archived under its phase. Its ONE "
        "citation (`fast-checks.yml:292`) is the evidence FOR the finding: the "
        "defect was that `referenced_scripts()` scraped a script name out of a "
        "COMMENT at that line. Re-anchoring it on a step name would delete the "
        "only thing that made the report checkable -- the comment is not in a "
        "step. Dated record of what line 292 said when the issue was filed",
    "docs/audit/p98-phase2/triage-dispositions.md":
        "dated p98 phase-2 disposition record. It quotes the same "
        "`fast-checks.yml:292` comment while explaining #311, for the same "
        "reason: the citation IS the finding. The document carries a "
        "HISTORICAL tracking banner and its own header says the tracker wins "
        "where they disagree",
    "docs/audit/p98-phase3/README.md":
        "dated p98 phase-3 fix record, and the SAME case as its phase-2 "
        "sibling above: it quotes the `fast-checks.yml:292` COMMENT because "
        "that comment IS #311's defect -- `referenced_scripts()` scraped a "
        "script name out of it -- so re-anchoring on a step name would delete "
        "the evidence (a comment is not in a step). Its second citation is "
        "this guard's own refusal text, quoted while explaining why the "
        "document had to be classified at all. Carries a HISTORICAL tracking "
        "banner. "
        "WHY IT WAS MISSED UNTIL THE 0.3.0 BUMP: the suite last read "
        "125/0/4 while docs/audit/p98-phase3/ was still UNTRACKED, and this "
        "guard only walks git-tracked files -- committing the record in "
        "c83dd12c is what made its citations visible. Nothing re-ran the "
        "suite between that commit and the next change, so a guard that was "
        "green before the commit was red after it, with no run in between to "
        "say so",
    "docs/audit/reverification-2026-08-10.md":
        "dated 2026-08-10 reverification of the audit-of-audits register; every "
        "workflow citation records what the lane looked like AT THAT READING, "
        "which is the whole point of the document -- several of its findings are "
        "precisely that an earlier claim no longer matched the file. Re-anchoring "
        "these on step names would destroy the evidence that a line moved",
    "docs/audit/meta-audit-2026-07/status-register.md":
        "audit-of-audits rows; the Evidence column is a dated phase observation",
    "docs/audit/meta-audit-2026-07/gap-register.md":
        "gap rows; the Evidence column is a dated phase-N observation",
    "docs/audit/meta-audit-2026-07/engine-security-gaps.md":
        "dated phase-4 engine-security audit artifact",
    "docs/audit/meta-audit-2026-07/evidence-audit-trail-phase-4.md":
        "dated phase-4 evidence trail; every row is an as-of reading",
    "docs/audit/meta-audit-2026-07/followup/deferred-backlog.md":
        "all citations here sit inside dated 'CLOSED <date> (Fnn, ...)' closure "
        "cells -- individually inspected in F43; they record what was true at "
        "closure, not what is true now",
    "docs/audit/meta-audit-2026-07/followup/phase-18-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/p95-phase12/criteria-status.md":
        "dated p95 phase-12 criteria record, classified during p96 phase-4 -- "
        "which is also when its numbers went stale, and by the hand of the "
        "person classifying it: adding one fast-checks.yml step shifted every "
        "line below it by 23, so the cited 606-607 / 628-629 are now wrong. "
        "That is exactly the case the HISTORICAL policy is for. The phase is "
        "closed and the citations record where those guards sat when the "
        "criteria were adjudicated; re-anchoring them on step names would read "
        "as if the phase had checked today's file, which it did not. The LIVE "
        "statement of where those guards are wired is the workflow itself",
    "docs/audit/p97-phase6/README.md":
        "dated p97 phase-6 record. Its `fast-checks.yml:148` citation is the "
        "line `check_coverage_floor.py --self-test` sat on when criterion 3 "
        "was adjudicated, which is what the document is evidence OF. It was "
        "already failing G3 before p97 phase-7 touched the workflow -- the "
        "file was never classified, so the guard was red on master with "
        "phase-6's gates green; found while adding phase-7's own step, which "
        "then shifted the line again and would have made the citation wrong "
        "as well as unclassified. Same shape as p95-phase12/criteria-status.md "
        "above, including the part where the shift was caused by the person "
        "doing the classifying. The phase is closed; re-anchoring on a step "
        "name would read as if phase-6 had checked today's file. The LIVE "
        "statement of where that guard is wired is the workflow itself",
    "docs/ci/repo-migration-2026-10/phase-3.md":
        "dated repo-migration phase-3 record. Its `fast-checks.yml:875` "
        "citation is the line `check_publication_block.py` is invoked on, and "
        "the point being made is precisely that the step runs the guard PLAINLY "
        "and never passes `--self-test` -- which is why 8 new cases went into "
        "the companion pytest file instead. The line number is the evidence, "
        "not an incidental reference. FOURTH occurrence of this shape in this "
        "list and the second by the same hand: a phase record quotes a workflow "
        "line, the guard refuses it, and the fix is always to classify rather "
        "than to re-anchor, because re-anchoring a closed phase's record on a "
        "step NAME would read as if the phase had checked today's file. The "
        "LIVE statement of where that guard is wired is the workflow itself.",
    "docs/audit/pub-readiness/phase-6/claims.md":
        "dated pub-readiness phase-6 claim table, written by the claim-check "
        "gate. Its `fast-checks.yml:1571` and `:875` citations are the lines "
        "test_secrets_baseline_arithmetic.py and check_publication_block.py "
        "sat on when the gate adjudicated claims 4 and 5 -- which is what the "
        "document is evidence OF, since the claim being checked was precisely "
        "'this guard is wired into the push lane'. THIRD occurrence of the "
        "shape recorded in this list (see p95-phase12/criteria-status.md and "
        "p97-phase6/README.md above), and the second where master went red "
        "with the phase's own gates all green: the file was copied into "
        "docs/ AFTER the phase's last local fast-checks sweep, so the sweep "
        "never saw it and the commit landed unverified. The sweep reproduces "
        "the failure exactly when re-run -- the evidence was stale, not "
        "wrong. The phase is closed; re-anchoring on step names would read as "
        "if claim-check had checked today's file. The LIVE statement of where "
        "those two guards are wired is the workflow itself",
    "docs/audit/audit-of-audits-2026-08.md":
        "the dated 2026-08 audit-of-audits; every workflow citation records "
        "what the lane looked like AT THAT READING, and several of its findings "
        "ARE that an earlier claim no longer matched the file -- re-anchoring "
        "would destroy the evidence that a line moved",
    "docs/audit/dca/0014-crl-issuer-keyusage-and-intermediate-ocsp.md":
        "a defect-cause-analysis record; a DCA is an as-of account of one "
        "defect and is never rewritten",
    "docs/audit/meta-audit-2026-07/followup/phase-63-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-66-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-73/fast_checks_local.log":
        "a CAPTURED RUN LOG. Editing it would forge the output of a command "
        "that already ran -- the citations in it are lines the tool printed, "
        "not claims the tree maintains",
    "docs/audit/meta-audit-2026-07/followup/phase-18/closed-test-verification.json":
        "dated per-phase evidence artifact",
    "docs/audit/meta-audit-2026-07/followup/phase-29-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-30-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-31-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-35-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-37-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-41-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-42-dispositions.md":
        "dated per-phase disposition record",
    "docs/audit/meta-audit-2026-07/followup/phase-43-dispositions.md":
        "this phase's own dated disposition record; it states the before/after "
        "line mapping, so it necessarily quotes both",

    # --- the e5b9c231 adversarial harness audit (phases 0-4), 'FIND-AND-DOCUMENT
    # only, no code changed'.  These are frozen findings from a completed audit,
    # and their governance.yml pointers were already long stale before F43 --
    # `governance.yml:51-58` now lands inside the F38 evidence comment block.
    # Rewriting a find-and-document register would falsify the audit.
    #
    # F43 found this whole group only because the mechanical sweep covered
    # `governance.yml:NNN` as well as `fast-checks.yml:NNN`; the hand-built
    # inventory this phase started from covered only the latter and missed all
    # six files.
    "docs/harness-audit/flaws-register.md":
        "frozen phase-0..4 adversarial harness-audit register (find-and-document)",
    "docs/harness-audit/flaws-register.json":
        "machine-readable twin of flaws-register.md; same frozen audit",
    "docs/harness-audit/phase-4/phase-4-synthesis.md":
        "dated phase-4 harness-audit synthesis (find-and-document)",
    "docs/harness-audit/phase-4/phase-4-synthesis.json":
        "machine-readable twin of phase-4-synthesis.md",
    "docs/harness-audit/phase-4/process-critique.md":
        "dated phase-4 process/governance critique (find-and-document)",
    "docs/harness-audit/phase-4/process-critique.json":
        "machine-readable twin of process-critique.md",
}

_WORKFLOW_BASENAMES = tuple(Path(p).name for p in TARGETS)

# `fast-checks.yml:92`, `.github/workflows/governance.yml:125-138`
#
# The lookbehind excludes only word chars and `-`, so `windows-governance.yml`
# does not match while the overwhelmingly common PATH-PREFIXED form
# `.github/workflows/fast-checks.yml:92` does.  An earlier revision of this
# checker also excluded `/` and `.` and therefore silently matched almost
# nothing -- caught because the sweep's hit count did not agree with a
# hand-derived inventory.
CITATION_RE = re.compile(
    r"(?<![\w-])(" + "|".join(re.escape(b) for b in _WORKFLOW_BASENAMES) + r"):(\d+(?:-\d+)?)"
)

# The LIVE-document anchor convention:   `fast-checks.yml` step *"NAME"*
#
# Matched over the WHOLE document, not line by line, and the captured name is
# whitespace-normalised before comparison, so prose may wrap a long step name
# across lines without silently falling out of the gate's sight.
ANCHOR_RE = re.compile(
    r"(" + "|".join(re.escape(b) for b in _WORKFLOW_BASENAMES) + r")`?\s+step\s+\*\"([^\"]+)\"\*"
)


def _norm(s: str) -> str:
    return " ".join(s.split())

_STEP_NAME_RE = re.compile(r"^\s+- name: (.*?)\s*$")
_NONCANCELLED_RE = re.compile(r"!\s*cancelled\s*\(\s*\)|(?<![\w.])always\s*\(\s*\)")
_CONTINUE_ON_ERROR_RE = re.compile(r"^\s*continue-on-error\s*:", re.MULTILINE)

# G2 EXTENDED TO windows-validation.yml (multi-runner work).
#
# G2's docstring has always cited windows-validation.yml as one of the files
# that "document two prior false greens from exactly that flag" -- but the rule
# only ever RAN over TARGETS, which is fast-checks.yml and governance.yml. The
# validation lane was never in scope. A comment in that lane asserting "this
# lane forbids it" was therefore describing a rule nothing enforced, which is
# the same shape as every other defect this file exists to catch.
#
# It is enforced here now, with an allowlist rather than a blanket ban, because
# the flag is not always wrong. It is catastrophic when it swallows a TEST
# failure (R-32, GAP-A15: a red suite reported green). It is correct on a step
# that gates nothing and whose failure resurfaces elsewhere. Each entry must say
# which case it is.
WV_CONTINUE_ON_ERROR_ALLOWED = {
    "Verify WinAppDriver version (PDFE-LIMITATION-WINAPPDRIVER-V0)":
        "Purely informational: records which WAD version the runner had. Gates "
        "nothing -- WAD-driven tests self-skip when the service is absent -- so "
        "its failure cannot hide a test result.",
    "Upload the published build as an ARTIFACT (cross-machine reuse)":
        "An optimisation. Nothing in this run consumes it; if the upload fails "
        "silently, the NEXT run's restore finds no artifact and refuses loudly "
        "('no published build for this commit'). The failure surfaces as a "
        "refusal later, never as a false green now.",
    "Upload the DEBUG closure as an ARTIFACT (cross-machine reuse)":
        "Same shape as the full-closure upload above, for the Debug-only store "
        "core-tests consumes. An optimisation: nothing in THIS run reads it, "
        "and a silent failure resurfaces as ensure-build-tree's loud "
        "cold-machine refusal on the run that needed it. The distinction that "
        "makes this safe and R-32/GAP-A15 catastrophic is not the flag -- it is "
        "that those swallowed a TEST result and this swallows a cache write.",
    "Generate release SBOM (SEC-95 / GitHub #71)":
        "Reporting, not a gate: it records what shipped, and nothing in this "
        "run or a later one consumes its output to make a pass/fail decision "
        "-- the actual CVE gate is the separate dependency-audit.yml lane "
        "(#50/#63), which is NOT continue-on-error. A release build must not "
        "go red because SBOM generation hit an unrelated hiccup; a missing "
        "SBOM is visible via the follow-on upload step's own "
        "if-no-files-found: warn, never silently green.",
    "Ship crash dumps to R2 (PRIVATE) — never to a GitHub artifact":
        "Post-mortem evidence, not a control. Nothing in this run reads it, and "
        "the crash it documents is reported by the job that crashed -- the dump "
        "is how you DIAGNOSE that failure, never how you DETECT it (detection is "
        "run_ui_suite.py's files_with_problems_count, which gates its own job). "
        "A swallowed upload therefore costs diagnosis on one run and cannot turn "
        "a red suite green, which is the distinction that made R-32 and GAP-A15 "
        "catastrophic and makes this safe. It also reaches an EXTERNAL store, so "
        "a hard failure here would redden the whole validation lane over a "
        "network or credential condition rather than a code one -- and the step "
        "already reports every unshipped file with ::warning::, so a silent loss "
        "is not among the outcomes.",
}


def gate_windows_validation_continue_on_error(problems, path=None):
    """G2 over windows-validation.yml: allowlisted steps only."""
    rel = WINDOWS_VALIDATION
    if path is None:
        path = REPO_ROOT / rel
    else:
        rel = str(path)
    steps = parse_steps_rich(path)
    text = _read(path)
    for m in _CONTINUE_ON_ERROR_RE.finditer(text):
        lineno = text[: m.start()].count("\n") + 1
        owner = None
        for ln, name, _c, _sid, _cm in steps:
            if ln <= lineno:
                owner = name
            else:
                break
        if owner in WV_CONTINUE_ON_ERROR_ALLOWED:
            continue
        problems.append(
            f"G2 {rel}:{lineno} step {owner!r} uses `continue-on-error`. In this "
            f"lane that flag has twice turned a red suite green (R-32, "
            f"GAP-A15). If the step gates nothing and its failure resurfaces "
            f"elsewhere, add it to WV_CONTINUE_ON_ERROR_ALLOWED with the reason; "
            f"otherwise remove the flag."
        )


class Failure(Exception):
    pass


def _read(path: Path) -> str:
    try:
        with open(path, "r", encoding="utf-8", newline=None) as fh:
            return fh.read()
    except OSError as exc:  # pragma: no cover - IO shape
        raise Failure(f"cannot read {path}: {exc}")


def parse_steps(path: Path):
    """Return [(line_number, name, if_expr_or_None)] in file order.

    Line-oriented on purpose: the whole point is to hand back the line number a
    human would see, which a YAML object graph does not carry.
    """
    text = _read(path)
    lines = text.split("\n")
    steps = []
    for idx, line in enumerate(lines, start=1):
        m = _STEP_NAME_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        indent = len(line) - len(line.lstrip())
        body_indent = " " * (indent + 2)
        cond = None
        for follow in lines[idx:]:
            if _STEP_NAME_RE.match(follow):
                break
            if follow.strip() and not follow.startswith(body_indent):
                break
            fm = re.match(re.escape(body_indent) + r"if:\s*(.*?)\s*$", follow)
            if fm:
                cond = fm.group(1)
        steps.append((idx, name, cond))
    if not steps:
        raise Failure(f"no `- name:` steps found in {path} (parser broken or file empty)")
    return steps


def _tracked_files():
    out = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-files"],
        capture_output=True, text=True, check=False,
    )
    if out.returncode != 0:
        raise Failure("`git ls-files` failed; this check must run inside the repo")
    return [p for p in out.stdout.split("\n") if p]


# --------------------------------------------------------------------------
# Gates
# --------------------------------------------------------------------------

def gate_guards_and_swallow(problems):
    """G1 + G2 + G6."""
    for rel, setup in TARGETS.items():
        path = REPO_ROOT / rel
        steps = parse_steps(path)
        names = {n for _, n, _ in steps}

        for lineno, name, cond in steps:
            if name in setup:
                if cond is not None:
                    problems.append(
                        f"G1 {rel}:{lineno} setup step {name!r} carries `if: {cond}`; "
                        "setup MUST stay fail-fast -- if the environment is absent the "
                        "checks below are meaningless."
                    )
                continue
            if cond is None:
                problems.append(
                    f"G1 {rel}:{lineno} check step {name!r} has NO `if:`. GitHub applies "
                    "an implicit success(), so an earlier red step SKIPS this control "
                    "(run 31242065496). Add `if: ${{ !cancelled() }}`."
                )
            elif not _NONCANCELLED_RE.search(cond):
                problems.append(
                    f"G1 {rel}:{lineno} check step {name!r} has `if: {cond}` with no "
                    "status function, so the implicit success() still applies and the "
                    "step is still masked. Use `!cancelled() && (<your condition>)`."
                )

        for m in _CONTINUE_ON_ERROR_RE.finditer(_read(path)):
            lineno = _read(path)[: m.start()].count("\n") + 1
            problems.append(
                f"G2 {rel}:{lineno} uses `continue-on-error`. The !cancelled() guard must "
                "change WHETHER A STEP RUNS, never whether its failure fails the job; "
                "swallowing the failure would be worse than the masking (R-32, GAP-A15)."
            )

        if rel.endswith("fast-checks.yml"):
            for name in REGRESSION_STEPS:
                if name not in names:
                    problems.append(
                        f"G6 {rel}: the step {name!r} -- one of the eight run 31242065496 "
                        "masked -- no longer exists under that name. Either restore it or "
                        "update REGRESSION_STEPS deliberately."
                    )


# --------------------------------------------------------------------------
# G7 -- windows-validation.yml, the F44 half of the same defect.
#
# WHY THIS IS NOT JUST ANOTHER ENTRY IN ``TARGETS``
# -------------------------------------------------
# ``TARGETS`` encodes the F43 policy: EVERY non-setup step carries a blanket
# ``!cancelled()``.  That policy is correct for fast-checks.yml and
# governance.yml, whose steps are mutually independent standing controls.  It is
# WRONG for windows-validation.yml, which is a build-then-test pipeline: several
# steps genuinely consume a prior step's build output, and running them after
# that build failed produces a confusing second red rather than a recovered
# signal.  Adding this workflow to ``TARGETS`` would also (a) demand a guard on
# ~15 build/setup steps that must stay fail-fast and (b) trip G2 on the
# deliberately best-effort "Verify WinAppDriver version" step.
#
# So F44 made NINE individual decisions, and this gate PINS them.  Each of the
# nine must name, in its own ``if:``:
#   * ``!cancelled()``            -- kill the implicit success() masking;
#   * a PRODUCER term             -- the specific step whose artefact it needs,
#                                    by ``steps.<id>.outcome``, so an UNRELATED
#                                    red step can no longer skip it;
#   * ``!inputs.ui_only``         -- the original subset-dispatch intent.
#
# Two producer relations appear, and the distinction is the substance of F44:
#   ``== 'success'``   the step needs a BINARY the producer builds.  No binary,
#                      nothing to run.
#   ``!= 'skipped'``   the step ADJUDICATES an artefact the producer writes
#                      before it fails.  It must run when the producer went RED
#                      -- that is the only run on which it says anything.
#                      Both instances are verified against the producing script,
#                      not assumed (run_gtest.py writes gtest-results.xml and
#                      healed_flakes.json regardless of outcome; run_ui_suite.py
#                      writes ui_test_matrix_aggregate.csv then `return 0`).
#
# NOT COVERED HERE, DELIBERATELY: windows-validation.yml is NOT added to the
# citation scope (G3/G4/G5).  A sweep found 107 `windows-validation.yml:NNN`
# citations across 26 tracked files; classifying all 26 LIVE/HISTORICAL is a
# real piece of work and doing it as a side effect of F44 would be the sort of
# unreviewed bulk edit this programme keeps paying for.  Recorded in
# docs/audit/meta-audit-2026-07/followup/phase-46-dispositions.md, not silently
# skipped.
# --------------------------------------------------------------------------

WINDOWS_VALIDATION = ".github/workflows/windows-validation.yml"

# Marker every one of the nine must carry in its comment block, so criterion 1's
# "every one of the nine has an inline comment stating which case it is and why"
# is machine-checked rather than eyeballed.
WV_MARKER = "F44 STEP-MASKING DECISION"

# F46. The marker a PRODUCER step must carry, stating why it is unconditional or
# what it is itself conditioned on. `gtest` is both a consumer and a producer and
# states its reasoning under the F44 marker, so either marker satisfies the rule.
WV_PRODUCER_MARKER = "F46 PRODUCER-GUARD DECISION"

# Any `steps.<id>.outcome` reference inside a condition, for deriving the
# producer set FROM THE FILE rather than from the pinned table -- a tenth
# consumer naming a sixth producer must be caught without editing this script.
_WV_PRODUCER_REF_RE = re.compile(r"steps\s*\.\s*([A-Za-z_][\w-]*)\s*\.\s*outcome")

# ...and the CROSS-JOB spelling of the same reference. Without this, the
# producer set derived from the file silently LOST build_debug, restore_build
# and build_release the moment the lane became a chained DAG -- so the half of
# G7 that checks "the producers the nine name are themselves guarded" stopped
# covering them, and stripping build_debug's guard no longer reddened anything.
# The mutation test caught exactly that, which is what it is for.
_WV_JOB_OUTPUT_RE = re.compile(
    r"needs\s*\.\s*[\w-]+\s*\.\s*outputs\s*\.\s*([A-Za-z_][\w-]*)")

# job-output name -> the step id that produces it.
_WV_OUTPUT_TO_STEP = {
    "built": "build_debug",
    "restored": "restore_build",
    "release_built": "build_release",
}


def producer_refs_in(cond: str) -> set:
    """Every producer STEP ID a condition names, in either spelling."""
    refs = set(_WV_PRODUCER_REF_RE.findall(cond or ""))
    for out in _WV_JOB_OUTPUT_RE.findall(cond or ""):
        sid = _WV_OUTPUT_TO_STEP.get(out)
        if sid:
            refs.add(sid)
    return refs

# step name -> (producer step id, comparison operator, expected outcome)
WV_DECISIONS = {
    "Run pdfe_tests":
        ("build_debug", "==", "success"),
    "Verify gtest JUnit XML (single-source checker — GAP-A15 / Register H6)":
        ("gtest", "!=", "skipped"),
    "Run + verify CROSS-PLATFORM CORE PARITY suite (C-10)":
        ("build_debug", "==", "success"),
    "Flaky-quarantine gate — reruns must not mask an untracked flake (R-37)":
        ("gtest", "!=", "skipped"),
    "Code coverage (OpenCppCoverage → Cobertura) — ENFORCED FLOOR (R-32)":
        ("build_debug", "==", "success"),
    "Performance-regression floor gate (C-07)":
        ("build_debug", "==", "success"),
    "Run ctest -L security (meta-audit F5 / SEC-23)":
        ("build_debug", "==", "success"),
    # MOVED OUT OF THE ui-e2e JOB, deliberately, and therefore out of the
    # in-job producer rule rather than silently dropped.
    #
    # The UI leg is now a 3-shard matrix, so there is no single `ui_e2e` step in
    # the same job for this gate to name; its dependency is expressed as
    # `needs: [ui-e2e]` at job level, and it scores the MERGED per-shard
    # aggregate via check_matrix_health.py --runtime-shard-dir.
    #
    # This is NOT a weakening. The old in-job step was guarded off whenever
    # ui_shards > 1 -- judging one shard against the whole closeout matrix would
    # fail every sharded run on the very condition sharding creates -- so under
    # sharding the gate previously stopped existing. Now it runs once, over all
    # shards, and `check_chained_jobs.py` enforces the job-level wiring that
    # replaced the step-level term.
    # (The matrix-health gate USED to be listed here as the ninth site, with
    # ("ui_e2e", "!=", "skipped"). It moved OUT of the ui-e2e job when that leg
    # became a 3-shard matrix, so there is no single `ui_e2e` step in its job to
    # name; the dependency is `needs: [ui-e2e]` at job level and it scores the
    # MERGED per-shard aggregate. Removed from this table DELIBERATELY, not
    # dropped -- see test_the_nine_is_actually_nine, and check_chained_jobs.py,
    # which enforces the job-level wiring that replaced the step-level term.
    #
    # NOT a weakening: the old in-job step was guarded off whenever
    # ui_shards > 1, so under sharding this gate previously stopped existing.)
    "Run render-verification suite (Release pdfe.exe)":
        ("build_release", "!=", None),   # placeholder, replaced below
}
# "Run ctest -L security" needs a SECOND producer (pytest); express the extra
# requirements separately so the common case stays a simple triple.
WV_EXTRA_PRODUCERS = {
    "Run ctest -L security (meta-audit F5 / SEC-23)": [("pydeps", "==", "success")],
}
WV_DECISIONS["Run render-verification suite (Release pdfe.exe)"] = (
    "build_release", "==", "success")

_WV_ID_RE = re.compile(r"^\s+id:\s*(\S+)\s*$")


def parse_steps_rich(path: Path):
    """[(line, name, cond, step_id, comment_block)] -- G7 needs id + comments.

    Kept separate from ``parse_steps`` so the F43 gates keep their exact
    behaviour.  Handles a FOLDED condition (``if: >-`` followed by indented
    continuation lines), which the simple parser would read as the literal
    string ``>-``.
    """
    text = _read(path)
    lines = text.split("\n")
    out = []
    for idx, line in enumerate(lines, start=1):
        m = _STEP_NAME_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        indent = len(line) - len(line.lstrip())
        body_indent = " " * (indent + 2)
        cond, step_id, comments = None, None, []
        follow_iter = list(enumerate(lines[idx:], start=idx + 1))
        for pos, (_j, follow) in enumerate(follow_iter):
            if _STEP_NAME_RE.match(follow):
                break
            if follow.strip() and not follow.startswith(body_indent):
                break
            if follow.strip().startswith("#"):
                comments.append(follow.strip())
                continue
            im = _WV_ID_RE.match(follow)
            if im and follow.startswith(body_indent + "id:"):
                step_id = im.group(1)
            fm = re.match(re.escape(body_indent) + r"if:\s*(.*?)\s*$", follow)
            if fm:
                cond = fm.group(1)
                if cond in (">", ">-", ">+", "|", "|-", "|+"):
                    # Folded/literal scalar: absorb the more-indented block.
                    parts = []
                    for _, cont in follow_iter[pos + 1:]:
                        if cont.strip() and not cont.startswith(body_indent + " "):
                            break
                        parts.append(cont.strip())
                    cond = " ".join(p for p in parts if p)
        out.append((idx, name, cond, step_id, "\n".join(comments)))
    if not out:
        raise Failure(f"no `- name:` steps found in {path} (parser broken or file empty)")
    return out


# When the producer sits in ANOTHER job, the same guarantee has to be spelled as
# a job OUTPUT -- `steps.<id>.outcome` referenced across a job boundary does not
# error, it evaluates to empty, which reads as "not success" and SILENTLY SKIPS
# the step. That is the F44/F46 masking class arriving through the chained-jobs
# refactor, so both spellings are accepted here and nothing else is.
#
# Outputs rather than `needs.<job>.result` deliberately: a consumer must know
# WHICH of build-vs-restore succeeded, and the job result cannot say.
WV_JOB_OUTPUT = {
    "build_debug": "needs.build.outputs.built",
    "restore_build": "needs.build.outputs.restored",
    "build_release": "needs.release.outputs.release_built",
}


def _producer_term_present(cond: str, sid: str, op: str, val: str) -> bool:
    """Whitespace-tolerant match for the step-level OR job-output spelling."""
    pat = (r"steps\s*\.\s*" + re.escape(sid) + r"\s*\.\s*outcome\s*"
           + re.escape(op) + r"\s*['\"]" + re.escape(val) + r"['\"]")
    if re.search(pat, cond) is not None:
        return True
    out = WV_JOB_OUTPUT.get(sid)
    if not out:
        return False
    jpat = (re.escape(out).replace(r"\.", r"\s*\.\s*") + r"\s*"
            + re.escape(op) + r"\s*['\"]" + re.escape(val) + r"['\"]")
    return re.search(jpat, cond) is not None


def gate_windows_validation(problems, path: Path | None = None):
    """G7. Every one of the nine F44 sites carries its pinned, commented decision."""
    rel = WINDOWS_VALIDATION
    if path is None:
        path = REPO_ROOT / rel
    else:
        rel = str(path)

    steps = parse_steps_rich(path)
    by_name = {n: (ln, c, sid, cm) for ln, n, c, sid, cm in steps}
    known_ids = {sid for _, _, _, sid, _ in steps if sid}

    for name, (pid, op, val) in WV_DECISIONS.items():
        if name not in by_name:
            problems.append(
                f"G7 {rel}: the step {name!r} -- one of the nine F44 sites -- no longer "
                "exists under that name. Either restore it or update WV_DECISIONS "
                "deliberately; a rename must not drop a step out of this gate's scope."
            )
            continue
        lineno, cond, _sid, comment = by_name[name]

        if cond is None:
            problems.append(
                f"G7 {rel}:{lineno} step {name!r} has NO `if:` at all. It is one of the "
                "nine F44 sites and must carry its reasoned condition."
            )
            continue

        if not _NONCANCELLED_RE.search(cond):
            problems.append(
                f"G7 {rel}:{lineno} step {name!r} has `if: {cond}` with NO status-check "
                "function, so GitHub's implicit success() still applies and ANY earlier "
                "red step silently SKIPS this control (F44, the windows-validation half "
                "of run 31242065496's defect). Add `!cancelled() && ...`."
            )

        wanted = [(pid, op, val)] + WV_EXTRA_PRODUCERS.get(name, [])
        for sid, sop, sval in wanted:
            if sid not in known_ids:
                problems.append(
                    f"G7 {rel}:{lineno} step {name!r} names producer id {sid!r}, but no "
                    "step in this workflow declares that id. GitHub evaluates an unknown "
                    "`steps.<id>.outcome` to null, so the condition is silently FALSE and "
                    "the step would never run again -- a worse failure than the masking."
                )
            elif not _producer_term_present(cond, sid, sop, sval):
                problems.append(
                    f"G7 {rel}:{lineno} step {name!r} must condition on "
                    f"`steps.{sid}.outcome {sop} '{sval}'` (it consumes that step's "
                    f"output) but its `if:` does not name it: `{cond}`."
                )

        if "!inputs.ui_only" not in cond.replace(" ", ""):
            problems.append(
                f"G7 {rel}:{lineno} step {name!r} dropped the `!inputs.ui_only` term. The "
                "F44 fix removes the MASKING, not the targeted-subset-dispatch intent."
            )

        if WV_MARKER not in comment:
            problems.append(
                f"G7 {rel}:{lineno} step {name!r} has no inline `{WV_MARKER}` comment. "
                "Criterion 1 of F44 requires every one of the nine to state IN THE FILE "
                "which case it is and why -- an unexplained condition is the thing that "
                "rots next."
            )

    # A tenth site appearing later must not slip past the pinned table.
    for lineno, name, cond, _sid, _cm in steps:
        if name in WV_DECISIONS:
            continue
        if cond is None:
            # NO `if:` AT ALL -- handled by _gate_wv_unconditional below, which
            # needs the step BODY to tell a verdict producer from environment
            # preparation. Until 2026-09-18 this loop `continue`d here, which
            # meant the ONE shape it exists to catch -- a new gate step with no
            # condition, to which GitHub then applies an implicit success() --
            # was the one shape it could not see (GitHub #205).
            continue
        if "inputs.ui_only" in cond and not _NONCANCELLED_RE.search(cond):
            problems.append(
                f"G7 {rel}:{lineno} step {name!r} carries `if: {cond}` -- a ui_only "
                "condition with no status function, i.e. a NEW instance of the F44 "
                "masking defect. Give it a reasoned condition and add it to WV_DECISIONS."
            )

    _gate_wv_unconditional(problems, rel, path, steps)
    _gate_wv_producers(problems, rel, steps)


# A step with NO `if:` runs under GitHub's implicit `success()`: any red step
# above it in the same job SKIPS it. Whether that is a defect depends entirely
# on what the step DOES, so this table is keyed on the step's BODY and never on
# its name -- a name allowlist would pass the next masked gate the moment
# somebody called it "Set up the coverage floor".
#
#   * A skipped ENVIRONMENT-PREPARATION step (checkout, toolchain env, fixture
#     download, cmake configure) hides nothing. It produces no verdict, and the
#     job is already red from whatever skipped it. F43 records that policy
#     deliberately: setup stays fail-fast.
#   * A skipped VERDICT/EVIDENCE step deletes signal. Its absence is reported
#     nowhere, the run's red is attributed to the first failure, and the check
#     that would have spoken is simply gone. That is F44, exactly.
#
# So the discriminator is "does this step produce or publish a verdict?", and
# these are the constructs by which a step in this workflow does that.
_WV_VERDICT_MARKERS = (
    ("tools/gate", "invokes a checker under tools/gate/"),
    ("tools\\gate", "invokes a checker under tools/gate/"),
    ("pytest", "runs a pytest suite"),
    ("ctest", "runs a ctest suite"),
    ("run_gtest", "runs the GoogleTest suite"),
    ("run_ui_suite", "runs the UI E2E suite"),
    ("run_render_suite", "runs the render suite"),
    ("pdfe_tests", "runs the GoogleTest binary"),
    ("GITHUB_STEP_SUMMARY", "publishes a job summary"),
    ("upload-artifact", "publishes an artifact"),
    ("download-artifact", "consumes a published artifact"),
)


def _step_bodies(path: Path, steps):
    """{start_line: code-only body} -- comment lines stripped.

    Comments are removed before matching so that a setup step whose *prose*
    mentions `pdfe_tests` (several do) is not misread as running it. The
    inverse -- a verdict step that hides its invocation in a comment -- is not
    a thing that can happen, because a commented-out invocation does not run.
    """
    lines = _read(path).split("\n")
    starts = [s[0] for s in steps] + [len(lines) + 1]
    out = {}
    for i, step in enumerate(steps):
        block = lines[step[0] - 1:starts[i + 1] - 1]
        out[step[0]] = "\n".join(
            ln for ln in block if not ln.strip().startswith("#"))
    return out


def _gate_wv_unconditional(problems, rel, path, steps):
    """G7, GitHub #205 half: a step with NO `if:` must be setup, not a gate.

    WHY THIS EXISTS. The tenth-site loop above only ever examined steps that
    HAD a condition -- it began `if name in WV_DECISIONS or cond is None:
    continue`. A gate step added with no `if:` line at all therefore slipped
    past it silently, and GitHub's implicit success() then masked that step
    exactly the way F44 masked the original nine. The detector could not see
    the shape it was written for. The F46 producer half did report a bare
    producer, but only for ids some OTHER condition already named, so a brand
    new unconditional step nobody references was invisible to both halves.

    THE RULE, AND WHY IT IS NOT AN ALLOWLIST. Every unconditional step is
    classified from its own body (see _WV_VERDICT_MARKERS). Twenty steps carry
    no `if:` in this workflow today -- seven checkouts, three copies each of the
    three toolchain-env actions, two fixture/SDK fetches, the ONNX model fetch
    and the cmake configure -- and not one of them matches a verdict marker, so
    this gate needs no per-step exception to pass the live tree. A new gate step
    added without a condition matches on the first thing it runs.

    A step id is the second signal: declaring one means some other step is
    meant to read this step's `outcome`, i.e. it is a producer by intent. F46
    guards producers, but only once a consumer names them; an unconditional
    step that declares an id before its consumer lands is caught here instead.
    """
    bodies = _step_bodies(path, steps)
    for lineno, name, cond, sid, _cm in steps:
        if cond is not None:
            continue
        body = bodies.get(lineno, "")
        for marker, what in _WV_VERDICT_MARKERS:
            if marker.lower() in body.lower():
                problems.append(
                    f"G7 {rel}:{lineno} step {name!r} has NO `if:` at all, yet it "
                    f"{what}. GitHub applies an implicit success() to a step with no "
                    "condition, so ANY red step above it in this job SILENTLY SKIPS "
                    "it -- the F44 masking defect in its original shape, on a step "
                    "that produces a VERDICT rather than environment. A skipped setup "
                    "step hides nothing; a skipped check deletes its own signal and "
                    "says nothing. Give it `if: ${{ !cancelled() && ... }}` with a "
                    "reasoned producer term, and add it to WV_DECISIONS."
                )
                break
        if sid is not None:
            problems.append(
                f"G7 {rel}:{lineno} step {name!r} has NO `if:` at all but DECLARES "
                f"id {sid!r}. An id exists so another step can read this one's "
                "`outcome`; under implicit success() an unrelated red above turns "
                "that outcome into 'skipped' and silently falsifies every condition "
                "that names it (F46, arriving before its consumer). Guard it."
            )


def _gate_wv_producers(problems, rel, steps):
    """G7, F46 half: every PRODUCER must itself be guarded.

    WHY THIS EXISTS.  F44 gave the nine consumers a `!cancelled()` plus a
    ``steps.<producer>.outcome`` term and declared the step-skip masking removed.
    It was not.  Four of the five producers those terms named carried NO ``if:``
    of their own, so GitHub's implicit ``success()`` applied to the PRODUCER: an
    unrelated red anywhere above it skipped the producer, ``outcome`` became
    ``'skipped'``, and every consumer naming it was skipped in turn.  The masking
    had been relocated one step upstream, and the original G7 -- checking only
    the nine consumers -- passed that workflow clean.  A gate that certifies the
    exact defect it exists to prevent is worse than no gate, because it is cited
    as evidence.

    THE RULE.  The producer set is derived FROM THE FILE (every ``steps.<id>``
    named by any condition), never from a hand-maintained list, so a tenth
    consumer naming a sixth producer is covered the day it lands.  Each producer
    must carry (a) a status-check function, so no upstream red can silently skip
    it, and (b) a marker comment recording whether it is unconditional or itself
    genuinely dependent, and why -- an unexplained guard is what rots next.
    A producer MAY legitimately carry a further producer term of its own
    (``ui_e2e`` needs ``build_debug``'s exe); that is fine, because the chain
    terminates at a guarded, unconditional producer.
    """
    by_id = {sid: (ln, n, c, cm) for ln, n, c, sid, cm in steps if sid}

    referenced = {}
    for lineno, name, cond, _sid, _cm in steps:
        if not cond:
            continue
        for pid in sorted(producer_refs_in(cond)):
            referenced.setdefault(pid, []).append(name)

    for pid in sorted(referenced):
        consumers = ", ".join(repr(c) for c in referenced[pid])
        if pid not in by_id:
            # The existing per-decision check reports this for the pinned nine;
            # this catches it for any other consumer naming an undeclared id.
            problems.append(
                f"G7 {rel}: producer id {pid!r} is named by {consumers} but NO step "
                "declares it. `steps.<unknown>.outcome` evaluates to null, so those "
                "conditions are permanently FALSE."
            )
            continue

        pline, pname, pcond, pcomment = by_id[pid]

        if pcond is None:
            problems.append(
                f"G7 {rel}:{pline} PRODUCER step {pname!r} (id {pid!r}, named by "
                f"{consumers}) has NO `if:` at all, so GitHub's implicit success() "
                "applies to IT: any red step above skips this producer, its outcome "
                "becomes 'skipped', and every consumer above is skipped with it. That "
                "is the F44 masking RELOCATED into the producer, not removed (F46). "
                "Give it `if: ${{ !cancelled() }}`, or an explicit producer term if it "
                "genuinely must not run after a specific upstream failure."
            )
        elif not _NONCANCELLED_RE.search(pcond):
            problems.append(
                f"G7 {rel}:{pline} PRODUCER step {pname!r} (id {pid!r}, named by "
                f"{consumers}) has `if: {pcond}` with NO status-check function, so "
                "implicit success() still lets an unrelated red skip it and, through "
                "it, every consumer that names it (F46). Add `!cancelled() && ...`."
            )

        if WV_PRODUCER_MARKER not in pcomment and WV_MARKER not in pcomment:
            problems.append(
                f"G7 {rel}:{pline} PRODUCER step {pname!r} (id {pid!r}, named by "
                f"{consumers}) has no inline `{WV_PRODUCER_MARKER}` comment. A "
                "producer's guard must state IN THE FILE whether it is unconditional "
                "or itself dependent, and why -- F44's producers were left silent and "
                "that silence is exactly how the relocated masking survived review."
            )


def gate_citations(problems):
    """G3 + G4 + G5."""
    classified = set(LIVE_DOCS) | set(HISTORICAL_DOCS)

    for rel in sorted(classified):
        if not (REPO_ROOT / rel).exists():
            problems.append(
                f"G3 classified document {rel} does not exist. The classification list "
                "has rotted; re-derive it rather than deleting the row."
            )

    step_names = {
        Path(rel).name: {n for _, n, _ in parse_steps(REPO_ROOT / rel)}
        for rel in TARGETS
    }

    for rel in _tracked_files():
        if rel in TARGETS or rel.startswith("tools/gate/"):
            continue  # the workflows themselves, and this checker + its self-test
        path = REPO_ROOT / rel
        if not path.is_file():
            continue
        # The sweep must cover EVERY tracked file, including ones that are not
        # clean UTF-8 (fixtures, legacy-encoded docs).  Decode with `replace`
        # rather than skipping: a `<workflow>.yml:NNN` citation is pure ASCII
        # and survives replacement, so nothing can hide behind an encoding
        # error.  Genuinely binary files (NUL byte) cannot contain a prose
        # citation and are skipped.
        try:
            blob = path.read_bytes()
        except OSError:
            continue
        if b"\x00" in blob:
            continue
        text = blob.decode("utf-8", errors="replace").replace("\r\n", "\n")

        for lineno, line in enumerate(text.split("\n"), start=1):
            for m in CITATION_RE.finditer(line):
                if rel in HISTORICAL_DOCS:
                    continue  # dated record; left as history, by policy
                if rel in LIVE_DOCS:
                    problems.append(
                        f"G4 {rel}:{lineno} carries the volatile citation "
                        f"`{m.group(1)}:{m.group(2)}`. {rel} is classified LIVE, so it must "
                        f'cite the step by name: `{m.group(1)}` step *"<exact step name>"*. '
                        "Resolve a current line number with "
                        "`python tools/gate/check_fast_checks_citations.py --list-steps`."
                    )
                else:
                    problems.append(
                        f"G3 {rel}:{lineno} cites `{m.group(1)}:{m.group(2)}` but {rel} is "
                        "classified neither LIVE nor HISTORICAL. Classify it in "
                        "check_fast_checks_citations.py: a dated record keeps its line "
                        "number, a live claim must be re-anchored on the step name."
                    )

        if rel in LIVE_DOCS:
            anchors = 0
            for m in ANCHOR_RE.finditer(text):
                anchors += 1
                wf, name = m.group(1), _norm(m.group(2))
                lineno = text[: m.start()].count("\n") + 1
                if name not in step_names[wf]:
                    problems.append(
                        f"G5 {rel}:{lineno} anchors on {wf} step {name!r}, which does not "
                        "exist. The step was renamed or removed; update the claim. Current "
                        "steps: `python tools/gate/check_fast_checks_citations.py "
                        "--list-steps`."
                    )
            if anchors == 0:
                problems.append(
                    f"G5 {rel} is classified LIVE but contains NO step-name anchor at all. "
                    "Either it no longer cites these workflows -- in which case remove it "
                    "from LIVE_DOCS -- or the anchor convention was written wrongly and "
                    "G4/G5 are now watching nothing."
                )


# --------------------------------------------------------------------------

def run_all():
    problems: list[str] = []
    gate_guards_and_swallow(problems)
    gate_windows_validation(problems)
    gate_windows_validation_continue_on_error(problems)
    gate_citations(problems)
    return problems


def main(argv=None):
    # Explicit UTF-8: step names carry em-dashes, and on Windows the default
    # console codepage (cp1252) mangles them -- which also makes the output
    # undecodable to any parent process capturing it as UTF-8.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):  # pragma: no cover - exotic stream
            pass

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--list-steps", action="store_true",
                    help="print `<line>\\t<name>` for every step in the target workflows")
    ap.add_argument("--resolve", metavar="NAME",
                    help="print the current line number of the step with this exact name")
    ap.add_argument("--workflow", default=None,
                    help="restrict --list-steps/--resolve to this workflow path")
    ap.add_argument("--wv-file", default=None, metavar="PATH",
                    help="run ONLY G7 (the F44 windows-validation step-masking gate) "
                         "against PATH instead of the tracked workflow. This is what "
                         "makes G7 discriminating: point it at "
                         "tools/gate/fixtures/windows-validation-unguarded-pre-f44.yml "
                         "and it must FAIL on all nine consumer sites, or at "
                         "tools/gate/fixtures/windows-validation-unguarded-producers-f46.yml "
                         "(consumers already fixed, producers still unguarded) and it "
                         "must FAIL on all four producers.")
    args = ap.parse_args(argv)

    try:
        if args.wv_file:
            problems = []
            gate_windows_validation(problems, Path(args.wv_file))
            if problems:
                print(f"[citations] G7 FAIL — {len(problems)} problem(s) in "
                      f"{args.wv_file}:")
                for p in problems:
                    print(f"  * {p}")
                return 1
            print(f"[citations] G7 OK — all {len(WV_DECISIONS)} pinned F44 sites in "
                  f"{args.wv_file} carry a guarded, producer-conditioned, commented "
                  "decision, AND every producer they name is itself guarded (F46).")
            return 0

        if args.list_steps or args.resolve:
            targets = [args.workflow] if args.workflow else list(TARGETS)
            hits = 0
            for rel in targets:
                path = Path(rel)
                if not path.is_absolute():
                    path = REPO_ROOT / rel
                if args.list_steps:
                    print(f"# {rel}")
                for lineno, name, cond in parse_steps(path):
                    if args.resolve:
                        if name == args.resolve:
                            print(f"{rel}:{lineno}\t{name}")
                            hits += 1
                    else:
                        print(f"{lineno}\t{name}")
            if args.resolve and hits == 0:
                print(f"[citations] no step named {args.resolve!r}", file=sys.stderr)
                return 1
            return 0

        problems = run_all()
    except Failure as exc:
        print(f"[citations] ERROR: {exc}", file=sys.stderr)
        return 2

    if problems:
        print(f"[citations] FAIL — {len(problems)} problem(s):")
        for p in problems:
            print(f"  * {p}")
        return 1

    n_steps = sum(len(parse_steps(REPO_ROOT / rel)) for rel in TARGETS)
    n_wv = len(parse_steps(REPO_ROOT / WINDOWS_VALIDATION))
    _wv_steps = parse_steps_rich(REPO_ROOT / WINDOWS_VALIDATION)
    n_prod = len({pid for _l, _n, c, _s, _cm in _wv_steps if c
                  for pid in producer_refs_in(c)})
    print(
        f"[citations] OK — {n_steps} steps across {len(TARGETS)} workflows: every check "
        f"step is guarded against step-skip masking, no continue-on-error, "
        f"{len(LIVE_DOCS)} live doc(s) name-anchored, "
        f"{len(HISTORICAL_DOCS)} dated record(s) left as history; "
        f"G7: all {len(WV_DECISIONS)} pinned F44 sites of "
        f"{Path(WINDOWS_VALIDATION).name} ({n_wv} steps) carry a guarded, "
        f"producer-conditioned, commented decision, and all {n_prod} producer "
        f"step(s) they name are themselves guarded (F46)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
