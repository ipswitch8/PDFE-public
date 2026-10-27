#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""The push/PR trigger topology is what it says it is (GitHub #187).

WHY THIS EXISTS
---------------
#187 is, at bottom, one sentence that went false and nothing noticed.
`windows-validation.yml`'s header said it "gates push/PR to master". That stopped
being true on 2026-07-20, when the `push`/`pull_request` triggers were removed to
keep the single self-hosted runner free. The sentence stayed, and for roughly two
months it read as reassurance — while ~8 gate scripts reachable only from that
file quietly ran nightly at best, and never on the commit that broke them.

Phase 1 of p85-remaining-open corrected the header to say the opposite: that the
workflow does NOT gate push or PR, and that `fast-checks.yml` is the only
workflow with those triggers.

**That replacement sentence had no mechanism behind it either.** The `claim-check`
gate found it by asking the only question that matters — *what fails if this
claim stops being true?* — and then checking: no script under `tools/gate/`
parsed any workflow's `on:` block. `check_fast_checks_citations.py` says so in
its own words: "Nothing here reads triggers." So a future commit re-adding
`push:` to a dispatch-only workflow, or removing it from `fast-checks.yml`, would
falsify the new header exactly as it falsified the old one, and the next audit
would find it two months later.

Writing a truer sentence is not a fix. This is the fix.

WHAT IS ASSERTED
----------------
1. **Topology.** Exactly the workflows in `EXPECTED_AUTO` carry an automatic
   trigger (`push`, `pull_request`, `pull_request_target`). Every other workflow
   must not. Both directions are distinct failures — see below.
2. **The claim names its enforcement.** A workflow whose header asserts it does
   not gate push/PR must cite this checker by filename, so a reader who doubts
   the sentence can run the thing instead of re-deriving the grep, and so the
   claim cannot drift away from the check that backs it.
3. **No comment DECLARES a schedule the `on:` block does not.** See below.

THE PROSE HALF (GitHub #205)
----------------------------
#187 was a header sentence that went false. #205 is the same defect one level
in: `windows-validation.yml`'s own `on:` block carried, in a single comment
block, both "NIGHTLY SCHEDULE + on-demand ... 04:20 UTC" and "NO SCHEDULED
TRIGGER — REMOVED 2026-08-13" — the stale claim FIRST, where a reader stops.
The original checker parsed the `on:` block and never read a word around it, so
the contradiction survived the very commit that added the guard.

**Only ONE prose shape is gated, deliberately.** 193 comment lines across these
14 workflows mention `schedule`/`cron`/`push`/`pull_request`, and nearly all of
them are discussing an ABSENCE ("NO `schedule:` TRIGGER — DELIBERATE", "do NOT
add a schedule trigger", "the push trigger was removed 2026-07-20"). Gating on
the presence of a trigger word would fail every workflow in the tree for saying
the right thing, and a negation heuristic keyed on phrasing ("no", "removed",
"never") is the kind of sweep that comes back the moment somebody writes a
sentence a different way. So the rule matches the DECLARATION shape only — a
cadence word, the word SCHEDULE, and a `+` joining it to a second trigger, which
is how all three real instances in this repository were written:

    # NIGHTLY SCHEDULE + on-demand (R2-14).                  windows-validation
    # MULTI-WEEKLY SCHEDULE + manual dispatch (F4, SEC-16)   windows-sanitizer
    # TRIGGER — WEEKLY SCHEDULE + dispatch, as of F5         mutation-kill

A cron-literal-in-a-comment rule was written and MEASURED as well, and dropped:
its only hit is `fuzz.yml`'s deliberately commented-out example of the trigger
that was removed, i.e. one false positive and zero true ones.

**Enforcement is a RATCHET, not a clean floor.** Two of the three instances are
live right now, in files p87 did not own: `windows-sanitizer.yml:27` and
`mutation-kill.yml:13` both declare a schedule this repository has not had since
2026-08-13, and both go on to describe crons (`0 18 * * 0,2,4`, Saturday 20:00)
that no `on:` block contains. `windows-sanitizer.yml` compounds it by citing
`check_asan_cadence.py` as enforcing "the cron beneath it" — there is no cron
beneath it. Those are real findings and they are NOT fixed here; making the rule
a clean floor would have meant either editing two files out of scope or
weakening the rule until it passed, and the second is how a gate becomes
decoration. `KNOWN_SCHEDULE_CLAIMS` below pins each one VERBATIM with a verdict.
A NEW declaration fails immediately; an entry whose text is edited or removed
also fails, so the list can only ratchet DOWN. It cannot quietly absorb the next
one.

FAILURE DIRECTIONS ARE NOT INTERCHANGEABLE
------------------------------------------
* `[unexpected-auto-trigger]` — a dispatch-only workflow GAINED push/PR. On this
  project that usually means a heavyweight self-hosted lane is about to run on
  every commit and saturate the one runner. Remedy: remove the trigger, or move
  the workflow into `EXPECTED_AUTO` deliberately and fix every header that says
  otherwise.
* `[missing-auto-trigger]` — a workflow that is SUPPOSED to gate every commit
  LOST its triggers. This is the more dangerous one and the quieter one: nothing
  goes red, CI simply stops gating, and every subsequent green means less than
  it appears to. Remedy: restore the trigger.

A single generic "topology mismatch" would leave the next person to work out
which of those two they hit. They have opposite remedies.

FAIL CLOSED EVERYWHERE
----------------------
Unparseable YAML, a missing `on:` block, an `on:` in a shape this cannot read, a
workflow directory with no workflows in it, or an expected workflow that has
disappeared — all non-zero. None of them is "probably fine".

Note `on:` is parsed by PyYAML as the BOOLEAN `True` (YAML 1.1 treats `on`/`off`
as booleans), not as the string "on". Reading only `d["on"]` finds nothing and
concludes every workflow is trigger-less — which would make this check pass
vacuously on the exact question it exists to answer. Both spellings are handled,
and the self-test pins it.

Usage:
    python tools/gate/check_workflow_triggers.py
    python tools/gate/check_workflow_triggers.py --self-test

Exit 0 = the topology and the claims agree, 1 = they do not, 2 = usage error.
"""
from __future__ import annotations

import argparse
import glob
import io
import os
import re
import shutil
import sys
import tempfile

try:
    import yaml
except ImportError:                                    # pragma: no cover
    sys.stderr.write("check_workflow_triggers: PyYAML is required "
                     "(pip install pyyaml)\n")
    raise

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_DIR = os.path.join(REPO_ROOT, ".github", "workflows")

#: Triggers that make a workflow run WITHOUT a human asking. `schedule` is
#: deliberately NOT here: a nightly is not a per-commit gate, and #187's whole
#: point is the difference between "runs eventually" and "blocks this commit".
AUTO_TRIGGERS = ("push", "pull_request", "pull_request_target")

#: The workflows that are SUPPOSED to run automatically. Changing this set is a
#: real decision about CI posture -- the single self-hosted Windows runner cannot
#: absorb a heavyweight lane on every commit -- so it lives in code, next to the
#: reasoning, rather than in a data file that reads as configuration.
EXPECTED_AUTO = ("fast-checks.yml",)

#: A workflow header that DENIES having automatic triggers must cite the checker
#: that backs the denial. Keyed by filename; the value is the phrase that marks
#: the claim, so a rewrite that drops the claim also drops the requirement.
CLAIM_SITES = {
    "windows-validation.yml": "DOES **NOT** GATE PUSH OR PR",
}

CHECKER_NAME = "check_workflow_triggers.py"

#: A comment line that DECLARES a scheduled trigger: a cadence word, the word
#: SCHEDULE, and a `+` joining it to the other trigger. Narrow on purpose -- see
#: "THE PROSE HALF" above for the shapes deliberately NOT matched and why.
SCHEDULE_CLAIM_RE = re.compile(
    r"#.*\b(?:NIGHTLY|DAILY|WEEKLY|MULTI-WEEKLY|BI-WEEKLY|FORTNIGHTLY|HOURLY|"
    r"MONTHLY)\s+SCHEDULE\b.*\+")

#: GitHub #244. The rule above keys on ONE syntactic shape, so a claim written
#: as an ordinary sentence -- "this workflow runs nightly at 04:20 UTC" -- is
#: not a member of the population and passes. That is the C-ENUM shape: a
#: control that enumerates one form reports "class closed" about the form, not
#: about the property.
#:
#: The extension stays SHAPE-based rather than becoming the keyword sweep the
#: header above rejects with measurements. It requires a present-tense
#: assertion that the workflow RUNS on a cadence -- a run verb bound to a
#: cadence word -- which is what a declaration looks like. Absence-talk ("NO
#: `schedule:` TRIGGER", "the push trigger was removed", "do NOT add a
#: schedule") has no such verb-cadence pair, which is why this does not
#: reproduce the flood the header measured: 193 comment lines mention
#: schedule/cron and this pattern matches none of them on the current tree.
#:
#: A negation guard is kept anyway, deliberately small, for the one shape that
#: WOULD otherwise collide: a sentence describing a cadence it is disclaiming
#: ("no longer runs nightly"). The header is right that negation heuristics
#: keyed on phrasing rot; this one is a narrowing of an already-narrow match,
#: not the basis of the rule.
_CADENCE = (r"(?:nightly|daily|weekly|hourly|monthly|fortnightly|"
            r"every\s+(?:night|day|week|hour|month))")
#: The SUBJECT must be this workflow. Without that clause the pattern matched 4
#: lines on the current tree and every one was a false positive -- "the daily
#: busy band as 03:30-13:30" (describing runner load), "Anything reachable only
#: from here runs nightly at best" (a statement about reachability), a quoted
#: phrase, and a remark about some other job. 4 false, 0 true: exactly the
#: flood the header predicted, measured rather than argued. A phantom schedule
#: claim is a claim about THIS file's own trigger, so that is what is required.
_SELF = r"(?:this\s+(?:workflow|file|lane|job|pipeline)|the\s+\w+\s+workflow)"
#: The gaps forbid `.` so a match cannot cross a sentence boundary. Without
#: that, "from this file. Anything reachable only from here runs nightly at
#: best" matched -- the subject of "runs" is "Anything", not "this file". One
#: false positive, found by measuring rather than by reading the regex.
SCHEDULE_PROSE_RE = re.compile(
    r"#.*\b" + _SELF + r"\b[^#.]{0,80}?"
    r"\b(?:runs?|executes?|fires?|is\s+scheduled|scheduled\s+to\s+run)\b"
    r"[^#.]{0,40}?\b" + _CADENCE + r"\b",
    re.IGNORECASE)
_SCHEDULE_NEGATION_RE = re.compile(
    r"\b(?:no\s+longer|never|not\s+|no\s+|removed|removal|deleted|disabled|"
    r"without|former(?:ly)?|previously|used\s+to|stopped|ceased|dropped)\b",
    re.IGNORECASE)


def _is_schedule_claim(line):
    """True when a comment line DECLARES a live scheduled trigger."""
    if SCHEDULE_CLAIM_RE.search(line):
        return True
    if SCHEDULE_PROSE_RE.search(line) and not _SCHEDULE_NEGATION_RE.search(line):
        return True
    return False

#: The declarations present when the prose rule landed (2026-09-18, GitHub
#: #205), pinned VERBATIM with a verdict each. Matching is exact on the stripped
#: line, so editing one of these lines -- in either direction -- fails until the
#: entry is updated or removed. A line that disappears must be deleted from here
#: too, which is what makes this ratchet down rather than accumulate.
KNOWN_SCHEDULE_CLAIMS = {
    # windows-sanitizer.yml and mutation-kill.yml USED TO BE DISPOSITIONED HERE.
    # Both entries are gone because GitHub #230 fixed the lines rather than
    # absorbing them, which is the direction this list is supposed to move.
    #
    # What changed in each file: the stale declaration was reworded to read as
    # history, the two false enforcement claims were corrected in place
    # (check_asan_cadence.py does not hold a comment to a cron that does not
    # exist; check_runner_queue_overlap.py has no live crons to re-derive from),
    # and mutation-kill.yml's claim that governance.yml runs "daily,
    # schedule-only" was corrected — governance.yml is dispatch-only like every
    # other lane. Neither file gained a `schedule:`.
    #
    # The surrounding cadence analysis was KEPT and marked historical, not
    # deleted: the slot arithmetic and the reason Saturday was excluded are the
    # inputs any future scheduling decision would need.
    "windows-fuzz.yml": [(
        '#   CORRECTED 2026-08-25. This header used to read "NIGHTLY SCHEDULE + manual',
        "NOT a defect -- this line QUOTES a claim it is in the act of "
        "retracting, and the retraction is the point of the paragraph. The rule "
        "cannot tell a quoted correction from a live declaration without a "
        "phrasing heuristic, so it is dispositioned here instead of being "
        "special-cased in the regex, where the next differently-worded "
        "correction would slip through it.",
    )],
}


def schedule_claims(workflow_dir):
    """{filename: [(lineno, stripped line)]} for every declaration-shaped claim
    in a workflow whose `on:` block declares no `schedule:`."""
    out = {}
    for path in sorted(glob.glob(os.path.join(workflow_dir, "*.yml"))
                       + glob.glob(os.path.join(workflow_dir, "*.yaml"))):
        name = os.path.basename(path)
        try:
            doc = yaml.safe_load(io.open(path, encoding="utf-8"))
        except Exception:
            continue          # the topology scan already fails this file
        _found, on_value = _on_block(doc)
        names = trigger_names(on_value)
        if names and "schedule" in names:
            continue          # the prose and the block agree
        text = io.open(path, encoding="utf-8", errors="replace").read()
        for i, line in enumerate(text.replace("\r\n", "\n").split("\n"), start=1):
            s = line.strip()
            if s.startswith("#") and _is_schedule_claim(s):
                out.setdefault(name, []).append((i, s))
    return out


def check_schedule_prose(workflow_dir, known=None):
    """Returns (messages, problems) for the GitHub #205 prose ratchet."""
    known = KNOWN_SCHEDULE_CLAIMS if known is None else known
    found = schedule_claims(workflow_dir)
    msgs, problems = [], []

    seen = set()
    for name in sorted(found):
        pinned = {t for t, _why in known.get(name, ())}
        for lineno, text in found[name]:
            if text in pinned:
                seen.add((name, text))
                msgs.append("  prose: %s:%d declaration DISPOSITIONED" % (name, lineno))
                continue
            problems.append(
                "[schedule-claimed-not-declared] %s:%d a comment DECLARES a "
                "scheduled trigger:\n      %s\n"
                "    but this workflow's `on:` block declares no `schedule:`. "
                "That is GitHub #205 exactly: windows-validation.yml carried a "
                "nightly-schedule paragraph directly above the line saying every "
                "schedule had been removed, and the topology check that ran on "
                "the same file read neither.\n"
                "    Remedy: delete the stale declaration, or -- if the schedule "
                "is genuinely wanted -- add it to the `on:` block. If the line is "
                "a QUOTED correction rather than a live claim, disposition it in "
                "KNOWN_SCHEDULE_CLAIMS in %s, with the reason."
                % (name, lineno, text, CHECKER_NAME))

    for name, entries in sorted(known.items()):
        for text, _why in entries:
            if (name, text) in seen:
                continue
            problems.append(
                "[schedule-claim-disposition-stale] %s no longer contains the "
                "pinned line:\n      %s\n"
                "    Either it was fixed -- in which case DELETE the entry from "
                "KNOWN_SCHEDULE_CLAIMS in %s, so the list ratchets down -- or it "
                "was reworded, in which case re-read it and re-pin the new text. "
                "A disposition list that keeps entries for lines that no longer "
                "exist stops being a record of anything."
                % (name, text, CHECKER_NAME))

    if not problems:
        msgs.append("  prose: no undispositioned schedule declaration in %d "
                    "dispositioned line(s)"
                    % sum(len(v) for v in known.values()))
    return msgs, problems


def _on_block(doc):
    """The `on:` value, whichever way YAML spelled the key.

    Returns (found, value). PyYAML parses a bare `on:` key as the boolean True
    under YAML 1.1, so `doc["on"]` is a KeyError on every real workflow here.
    """
    if not isinstance(doc, dict):
        return False, None
    for key in doc:
        if key is True or (isinstance(key, str) and key.strip().lower() == "on"):
            return True, doc[key]
    return False, None


def trigger_names(on_value):
    """The set of trigger names in an `on:` block, or None if unreadable.

    GitHub accepts three shapes: a mapping, a list, and a bare string. All three
    are handled; anything else returns None so the caller fails closed rather
    than treating an unrecognised shape as "no triggers".
    """
    if isinstance(on_value, dict):
        return {str(k).strip().lower() for k in on_value}
    if isinstance(on_value, list):
        return {str(v).strip().lower() for v in on_value}
    if isinstance(on_value, str):
        return {on_value.strip().lower()}
    return None


def auto_triggers_of(on_value):
    names = trigger_names(on_value)
    if names is None:
        return None
    return sorted(n for n in names if n in AUTO_TRIGGERS)


def scan(workflow_dir):
    """filename -> {'auto': [...]} or {'error': '...'}. Never raises on content."""
    out = {}
    for path in sorted(glob.glob(os.path.join(workflow_dir, "*.yml"))
                       + glob.glob(os.path.join(workflow_dir, "*.yaml"))):
        name = os.path.basename(path)
        try:
            doc = yaml.safe_load(io.open(path, encoding="utf-8"))
        except Exception as exc:
            out[name] = {"error": "YAML will not parse: %s" % str(exc)[:160]}
            continue
        found, on_value = _on_block(doc)
        if not found:
            out[name] = {"error": "no `on:` block — a workflow with no triggers "
                                  "cannot be reasoned about"}
            continue
        auto = auto_triggers_of(on_value)
        if auto is None:
            out[name] = {"error": "`on:` is a %s, which this checker cannot read. "
                                  "Refusing to guess." % type(on_value).__name__}
            continue
        out[name] = {"auto": auto}
    return out


def check(workflow_dir=DEFAULT_DIR, expected=EXPECTED_AUTO,
          claim_sites=None, out=sys.stdout, known_claims=None):
    """Returns (ok, messages). Messages are printed to `out` as well."""
    claim_sites = CLAIM_SITES if claim_sites is None else claim_sites
    known_claims = KNOWN_SCHEDULE_CLAIMS if known_claims is None else known_claims
    msgs = []
    problems = []

    if not os.path.isdir(workflow_dir):
        msg = ("FAIL [workflows-missing] %s is not a directory. There is nothing "
               "to check, which is not the same as everything being correct."
               % workflow_dir)
        print(msg, file=out)
        return False, [msg]

    results = scan(workflow_dir)
    if not results:
        msg = ("FAIL [no-workflows] %s contains no .yml/.yaml files. This check "
               "would otherwise pass over an empty set — a green that means "
               "nothing." % workflow_dir)
        print(msg, file=out)
        return False, [msg]

    msgs.append("=== automatic-trigger topology (%d workflow(s)) ===" % len(results))

    for name in sorted(results):
        r = results[name]
        if "error" in r:
            problems.append("[workflow-unreadable] %s: %s" % (name, r["error"]))
            continue
        auto = r["auto"]
        expected_here = name in expected
        if auto:
            msgs.append("  %-30s AUTO: %s" % (name, ", ".join(auto)))
        else:
            msgs.append("  %-30s dispatch/schedule only" % name)

        if auto and not expected_here:
            problems.append(
                "[unexpected-auto-trigger] %s carries %s but is not in the "
                "expected auto-run set %s.\n"
                "    A heavyweight or self-hosted lane running on every commit "
                "saturates the single Windows runner, and — the #187 failure — "
                "every header that says this workflow does not gate push/PR is "
                "now FALSE.\n"
                "    Remedy: remove the trigger, or add this workflow to "
                "EXPECTED_AUTO in %s deliberately AND fix every header that "
                "claims otherwise."
                % (name, "/".join(auto), list(expected), CHECKER_NAME))
        if expected_here and not auto:
            problems.append(
                "[missing-auto-trigger] %s is expected to run on every push/PR "
                "but carries NONE of %s.\n"
                "    This is the quieter and more dangerous direction: nothing "
                "goes red, CI simply stops gating commits, and every green after "
                "this point means less than it appears to.\n"
                "    Remedy: restore the trigger."
                % (name, "/".join(AUTO_TRIGGERS)))

    for name in expected:
        if name not in results:
            problems.append(
                "[expected-workflow-absent] %s is named in EXPECTED_AUTO but does "
                "not exist in %s. The per-commit gate cannot be missing and the "
                "topology still be correct." % (name, workflow_dir))

    # ---- the claim must name its enforcement -------------------------------
    for name, phrase in sorted(claim_sites.items()):
        path = os.path.join(workflow_dir, name)
        if not os.path.exists(path):
            problems.append(
                "[claim-site-absent] %s is registered as making a trigger claim "
                "but does not exist. Remove it from CLAIM_SITES or restore the "
                "file." % name)
            continue
        text = io.open(path, encoding="utf-8", errors="replace").read()
        if phrase not in text:
            problems.append(
                "[claim-text-changed] %s no longer contains the registered claim "
                "%r. Either the header was rewritten — in which case re-register "
                "the new wording — or the claim was dropped and this entry should "
                "go. A claim site that has silently stopped claiming anything is "
                "how #187 happened." % (name, phrase))
            continue
        if CHECKER_NAME not in text:
            problems.append(
                "[claim-uncited] %s asserts it does not gate push/PR but never "
                "names %s, the check that enforces it. A reader who doubts the "
                "sentence has to re-derive the grep, and the claim can drift away "
                "from its enforcement unnoticed. Cite the checker in the header."
                % (name, CHECKER_NAME))
        else:
            msgs.append("  claim in %s cites %s" % (name, CHECKER_NAME))

    # ---- GitHub #205: no comment may DECLARE a schedule the block does not --
    prose_msgs, prose_problems = check_schedule_prose(workflow_dir, known_claims)
    msgs.extend(prose_msgs)
    problems.extend(prose_problems)

    for p in problems:
        msgs.append("FAIL %s" % p)

    for m in msgs:
        print(m, file=out)

    if problems:
        print("check_workflow_triggers: FAIL — %d problem(s)." % len(problems),
              file=out)
        return False, msgs
    print("check_workflow_triggers: PASS — %d workflow(s) scanned; exactly %s "
          "carr%s an automatic trigger, every other workflow is "
          "dispatch/schedule-only, and each registered claim cites this checker."
          % (len(results), list(expected), "ies" if len(expected) == 1 else "y"),
          file=out)
    return True, msgs


# --------------------------------------------------------------------------
# self-test — written BEFORE this was wired into the push lane, which is the
# lesson the --check defect in this same phase taught: a check promoted into a
# blocking lane on "exit 0, offline, fast" with no negative case of its own is
# how a claim outruns its mechanism.
# --------------------------------------------------------------------------

_DISPATCH = "name: X\non:\n  workflow_dispatch:\n\njobs:\n  a:\n    runs-on: ubuntu-latest\n"
_AUTO = ("name: Y\non:\n  push:\n    branches: [master]\n  pull_request:\n"
         "    branches: [master]\n  workflow_dispatch:\n\njobs:\n  a:\n"
         "    runs-on: ubuntu-latest\n")
_AUTO_LIST = "name: Z\non: [push, workflow_dispatch]\n\njobs:\n  a:\n    runs-on: ubuntu-latest\n"
_AUTO_STR = "name: Z\non: push\n\njobs:\n  a:\n    runs-on: ubuntu-latest\n"
_NO_ON = "name: W\njobs:\n  a:\n    runs-on: ubuntu-latest\n"
_BAD_YAML = "name: V\non:\n  push:\n   - [unbalanced\n"
_ON_SCALAR_INT = "name: U\non: 7\n\njobs:\n  a:\n    runs-on: ubuntu-latest\n"


def self_test():
    failures = 0

    def check_case(label, got, want):
        nonlocal failures
        ok = got == want
        if not ok:
            failures += 1
        print("  [%s] %-60s got=%r" % ("ok  " if ok else "FAIL", label, got))

    devnull = io.StringIO()
    tmp = tempfile.mkdtemp(prefix="wf_triggers_selftest_")

    def tree(files, name=None):
        d = tempfile.mkdtemp(dir=tmp)
        for fn, body in files.items():
            io.open(os.path.join(d, fn), "w", encoding="utf-8",
                    newline="\n").write(body)
        return d

    try:
        # --- the YAML 1.1 trap, pinned directly. ---------------------------
        doc = yaml.safe_load(_DISPATCH)
        check_case("PyYAML really does parse `on:` as the boolean True",
                   True in doc, True)
        check_case("_on_block finds it anyway", _on_block(doc)[0], True)
        check_case("and a quoted \"on\" key is found too",
                   _on_block(yaml.safe_load('name: X\n"on":\n  push:\n'))[0], True)

        # --- shape handling, all three legal forms + a refusal. -------------
        check_case("mapping form is read", auto_triggers_of({"push": None,
                                                             "workflow_dispatch": None}),
                   ["push"])
        check_case("list form is read", auto_triggers_of(["push", "workflow_dispatch"]),
                   ["push"])
        check_case("bare-string form is read", auto_triggers_of("push"), ["push"])
        check_case("an unreadable shape returns None, not an empty set",
                   auto_triggers_of(7), None)
        check_case("schedule is NOT an automatic trigger for this purpose",
                   auto_triggers_of({"schedule": None}), [])

        # --- the happy path ------------------------------------------------
        d = tree({"fast-checks.yml": _AUTO, "heavy.yml": _DISPATCH})
        ok, _ = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("correct topology PASSES", ok, True)

        # --- DIRECTION 1: a dispatch-only workflow gains push --------------
        d = tree({"fast-checks.yml": _AUTO, "heavy.yml": _AUTO})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("a dispatch-only workflow GAINING push FAILS", ok, False)
        check_case("and it is reported as [unexpected-auto-trigger]",
                   any("[unexpected-auto-trigger] heavy.yml" in m for m in msgs), True)

        # ...including via the list and bare-string spellings.
        for label, body in (("list", _AUTO_LIST), ("bare string", _AUTO_STR)):
            d = tree({"fast-checks.yml": _AUTO, "heavy.yml": body})
            ok, _ = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
            check_case("...also caught when written as a %s" % label, ok, False)

        # --- DIRECTION 2: fast-checks loses its triggers -------------------
        d = tree({"fast-checks.yml": _DISPATCH, "heavy.yml": _DISPATCH})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("fast-checks LOSING push/PR FAILS", ok, False)
        check_case("and it is reported as [missing-auto-trigger]",
                   any("[missing-auto-trigger] fast-checks.yml" in m for m in msgs),
                   True)
        check_case("and the two directions are DIFFERENT messages",
                   any("[unexpected-auto-trigger]" in m for m in msgs), False)

        # --- fail-closed paths ---------------------------------------------
        d = tree({"fast-checks.yml": _AUTO, "broken.yml": _BAD_YAML})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("UNPARSEABLE yaml FAILS", ok, False)
        check_case("and is reported as [workflow-unreadable]",
                   any("[workflow-unreadable] broken.yml" in m for m in msgs), True)

        d = tree({"fast-checks.yml": _AUTO, "noon.yml": _NO_ON})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("a workflow with NO `on:` block FAILS", ok, False)

        d = tree({"fast-checks.yml": _AUTO, "weird.yml": _ON_SCALAR_INT})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("an `on:` in an unreadable SHAPE FAILS", ok, False)

        d = tree({"heavy.yml": _DISPATCH})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("the expected per-commit workflow being ABSENT FAILS", ok, False)
        check_case("and is reported as [expected-workflow-absent]",
                   any("[expected-workflow-absent]" in m for m in msgs), True)

        d = tree({})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("an EMPTY workflow dir FAILS rather than passing vacuously",
                   ok, False)
        ok, _ = check(os.path.join(tmp, "definitely-absent"), ("fast-checks.yml",),
                      {}, out=devnull, known_claims={})
        check_case("a MISSING workflow dir FAILS", ok, False)

        # --- the claim-cites-its-enforcement rule, both ways ---------------
        claim = {"heavy.yml": "DOES NOT GATE"}
        d = tree({"fast-checks.yml": _AUTO,
                  "heavy.yml": "# DOES NOT GATE push/PR; enforced by "
                               + CHECKER_NAME + "\n" + _DISPATCH})
        ok, _ = check(d, ("fast-checks.yml",), claim, out=devnull, known_claims={})
        check_case("a claim that CITES the checker PASSES", ok, True)

        d = tree({"fast-checks.yml": _AUTO,
                  "heavy.yml": "# DOES NOT GATE push/PR, trust me\n" + _DISPATCH})
        ok, msgs = check(d, ("fast-checks.yml",), claim, out=devnull, known_claims={})
        check_case("a claim that does NOT cite the checker FAILS", ok, False)
        check_case("and is reported as [claim-uncited]",
                   any("[claim-uncited] heavy.yml" in m for m in msgs), True)

        d = tree({"fast-checks.yml": _AUTO, "heavy.yml": _DISPATCH})
        ok, msgs = check(d, ("fast-checks.yml",), claim, out=devnull, known_claims={})
        check_case("a claim site whose CLAIM TEXT vanished FAILS", ok, False)
        check_case("and is reported as [claim-text-changed]",
                   any("[claim-text-changed] heavy.yml" in m for m in msgs), True)

        # --- and the real tree, so this is not only synthetic --------------
        # The REAL disposition registry, deliberately: every synthetic case
        # above passes `known_claims={}` because a temp dir cannot contain the
        # pinned lines, but this one must exercise the shipped list or the
        # ratchet would be self-tested only against an empty set.
        ok, _ = check(out=devnull)
        check_case("the REAL .github/workflows topology holds", ok, True)

        # --- GitHub #205: the prose ratchet, both directions ---------------
        # THE CONTROL IS THE PRE-FIX FILE ITSELF. windows-validation.yml's `on:`
        # block carried the NIGHTLY SCHEDULE paragraph until 2026-09-18; if the
        # rule does not redden that exact text, it is decoration.
        _PRE_205 = (
            "name: X\non:\n"
            "  # NIGHTLY SCHEDULE + on-demand (R2-14). A bounded nightly run gives the\n"
            "  # suite a real, unattended trigger so regressions surface within a day.\n"
            "  # NO SCHEDULED TRIGGER — REMOVED 2026-08-13, DELIBERATELY.\n"
            "  workflow_dispatch:\n\njobs:\n  a:\n    runs-on: ubuntu-latest\n")
        d = tree({"fast-checks.yml": _AUTO, "pre205.yml": _PRE_205})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("the PRE-#205 nightly paragraph FAILS", ok, False)
        check_case("and is reported as [schedule-claimed-not-declared]",
                   any("[schedule-claimed-not-declared] pre205.yml" in m
                       for m in msgs), True)

        # Deleting the stale paragraph -- the fix that was actually applied --
        # must clear it. A finding that its own prescribed remedy does not
        # answer is noise.
        d = tree({"fast-checks.yml": _AUTO,
                  "post205.yml": _PRE_205.replace(
                      "  # NIGHTLY SCHEDULE + on-demand (R2-14). A bounded nightly run gives the\n"
                      "  # suite a real, unattended trigger so regressions surface within a day.\n",
                      "")})
        ok, _ = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("deleting the stale paragraph CLEARS it", ok, True)

        # The other legitimate remedy: actually declare the schedule.
        d = tree({"fast-checks.yml": _AUTO,
                  "real.yml": _PRE_205.replace(
                      "  workflow_dispatch:\n",
                      "  schedule:\n    - cron: '20 4 * * *'\n  workflow_dispatch:\n")})
        ok, _ = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("declaring the schedule the prose claims CLEARS it", ok, True)

        # Prose that DENIES a schedule must not trip the rule -- 193 comment
        # lines in this repository do exactly that, and a rule that reddened
        # them would be turned off within a week.
        _DENIALS = (
            "name: X\non:\n"
            "  # NO `schedule:` TRIGGER — DELIBERATE. Do NOT add a schedule trigger.\n"
            "  # The push trigger was removed 2026-07-20; nightly runs are gone.\n"
            "  # Every scheduled trigger in this repository was removed 2026-08-13.\n"
            "  workflow_dispatch:\n\njobs:\n  a:\n    runs-on: ubuntu-latest\n")
        d = tree({"fast-checks.yml": _AUTO, "denials.yml": _DENIALS})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull, known_claims={})
        check_case("prose DENYING a schedule stays green", ok, True)

        # The ratchet: a dispositioned line passes; the SAME line once the
        # disposition is dropped fails; and a disposition for a line that is
        # gone fails too, so the list cannot accumulate dead entries.
        pinned = "# NIGHTLY SCHEDULE + on-demand (R2-14). A bounded nightly run gives the"
        d = tree({"fast-checks.yml": _AUTO, "pre205.yml": _PRE_205})
        ok, _ = check(d, ("fast-checks.yml",), {}, out=devnull,
                      known_claims={"pre205.yml": [(pinned, "dispositioned")]})
        check_case("a DISPOSITIONED declaration passes", ok, True)

        d = tree({"fast-checks.yml": _AUTO, "clean.yml": _DENIALS})
        ok, msgs = check(d, ("fast-checks.yml",), {}, out=devnull,
                         known_claims={"clean.yml": [(pinned, "dispositioned")]})
        check_case("a disposition whose LINE IS GONE fails", ok, False)
        check_case("and is reported as [schedule-claim-disposition-stale]",
                   any("[schedule-claim-disposition-stale] clean.yml" in m
                       for m in msgs), True)

        # And the live windows-validation.yml, by name: the file this issue is
        # about must carry no undispositioned declaration.
        check_case("the LIVE windows-validation.yml declares no phantom schedule",
                   schedule_claims(DEFAULT_DIR).get("windows-validation.yml", []),
                   [])

        # --- GitHub #244: a claim written as ordinary English. -------------
        # The original rule keyed on ONE shape (cadence + SCHEDULE + `+`), so a
        # sentence was not a member of the population and passed. These cases
        # pin BOTH directions: the English declaration must be caught, and the
        # lines that merely MENTION a cadence must not be, because a keyword
        # sweep here was measured at 4 false positives and 0 true ones.
        english = tree({"prose.yml":
                        "# This workflow runs nightly at 04:20 UTC.\n"
                        "on:\n  workflow_dispatch:\n"})
        check_case("an ENGLISH phantom-schedule claim is caught",
                   [t for _ln, t in schedule_claims(english).get("prose.yml", [])],
                   ["# This workflow runs nightly at 04:20 UTC."])

        check_case("...and so is the 'is scheduled to run' phrasing",
                   bool(schedule_claims(tree({"p2.yml":
                        "# this file is scheduled to run weekly (F4).\n"
                        "on:\n  workflow_dispatch:\n"})).get("p2.yml")),
                   True)

        check_case("a DENIAL naming the same cadence is not a claim",
                   schedule_claims(tree({"p3.yml":
                        "# This workflow no longer runs nightly -- removed.\n"
                        "on:\n  workflow_dispatch:\n"})).get("p3.yml", []),
                   [])

        check_case("a cadence mentioned about something else is not a claim",
                   schedule_claims(tree({"p4.yml":
                        "# It gave the daily busy band as 03:30-13:30.\n"
                        "on:\n  workflow_dispatch:\n"})).get("p4.yml", []),
                   [])

        # The measured false positive that forced the sentence-boundary guard:
        # the subject of "runs" is "Anything", not "this file".
        check_case("a cadence in a NEW SENTENCE after a self-reference is not a claim",
                   schedule_claims(tree({"p5.yml":
                        "#   from this file. Anything reachable only from here "
                        "runs nightly at best, and\n"
                        "on:\n  workflow_dispatch:\n"})).get("p5.yml", []),
                   [])

        # And a workflow that DOES declare a schedule must not be flagged for
        # describing it truthfully.
        check_case("an English cadence WITH a real schedule block is fine",
                   schedule_claims(tree({"p6.yml":
                        "# This workflow runs nightly at 04:20 UTC.\n"
                        "on:\n  schedule:\n    - cron: '20 4 * * *'\n"})).get("p6.yml", []),
                   [])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("PASS" if failures == 0 else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--workflow-dir", default=DEFAULT_DIR)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    ok, _ = check(args.workflow_dir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
