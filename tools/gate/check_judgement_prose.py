# -*- coding: utf-8 -*-
"""A per-file judgement record may not contradict its own classification.

WHY THIS EXISTS
---------------
`docs/ci/repo-migration-2026-10/phase-5-file-judgements.md` records, per file,
whether that file needed a change. The `claim-check` gate FAILED phase-5 twice
over this record:

1. A row was classified `no change` while the diff modified the file. A
   check already existed for that -- it compares the CLASSIFICATION against
   `git status`, in both directions.

2. A row was correctly classified `**CHANGE**` while its *prose* still said
   "the version badge left alone because it reports 0.3.0 correctly off the
   live repository". Both halves false, in the document that is supposed to be
   authoritative, after the narrative record had already corrected it.

Nothing compared a judgement's PROSE against its classification. That is this
checker. It is narrow on purpose: it does not adjudicate prose in general, it
catches a row that is marked CHANGE while asserting the file was left alone.

WHY IT IS TRACKED AND TESTED RATHER THAN A SCRATCH SCRIPT
---------------------------------------------------------
The first version of this logic lived in the Claude job's temp directory. It
ran once, by hand, to produce the markdown -- and `claim-check` FAILED the
phase again for precisely that: the remediation claimed "there is now a check
… the generator refuses to write the record", while `git ls-files` showed zero
hits for its distinguishing symbols. A control that exists only in scratch
space is not a control, and describing it as one is the same defect it was
written to prevent. This is the second time in this pipeline that an untracked
script was credited as a standing mechanism (phase-4's rename preflight was the
first), which is why the test asserts this file is tracked.

ON THE LIMITS OF THE MATCHING, STATED PLAINLY
---------------------------------------------
This is a phrase/regex heuristic over negation-of-change wording. `claim-check`
named five synonyms the first phrase list missed ("remains as it was", "is
unchanged", "kept as is", "stays the same", "needs no modification"); all are
covered now. It is still a heuristic and a sufficiently creative paraphrase
will evade it. It is therefore a SECOND line of defence -- the classification
check against `git status` remains the primary one, and it is not
wording-dependent at all.

**It also false-positives on prose that DESCRIBES the rule.** Measured the
first time this checker ran for real: it flagged the judgement-record entry for
its own source file, because that entry restated the rule using the wording the
rule forbids. It matches wording, so "may not claim the file was left alone"
and "the file was left alone" look the same to it.

Both known failure directions are therefore live: a creative paraphrase gets
through, and a description of the rule gets stopped. The second is the better
one to have -- it fails closed, refusing to write rather than passing something
false -- and it is why this is explicitly the second line of defence.

The primary control is `check_record_vs_diff.py`, which compares the record's
classifications against `git status` and reads no prose at all.

> That sentence was false when first written here. The record/diff logic then
> lived only in a Claude job's temp directory, and `claim-check` failed phase-5
> for citing it as a standing control from this very docstring. It is tracked
> and CI-wired now. The lesson is recorded in
> `docs/ci/repo-migration-2026-10/phase-5.md` section 8: naming a failure mode
> is no protection against repeating it -- that was the third untracked
> "mechanism" in one pipeline, written inside the sentence excusing this file's
> weakness.
"""
import argparse
import io
import os
import re
import sys

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_RECORDS = (
    os.path.join("docs", "ci", "repo-migration-2026-10",
                 "phase-5-file-judgements.md"),
)

# Negation-of-change wording. Built from what was actually observed plus the
# synonyms `claim-check` named. Stems rather than whole phrases where possible,
# so "remain/remains/remained" and "stay/stays/stayed" all match.
NO_CHANGE_PATTERNS = (
    r"left\s+alone",
    r"left\s+(?:as|unchanged|untouched|in\s+place)",
    r"kept\s+(?:as|unchanged|untouched|the\s+same)",
    r"remain(?:s|ed)?\s+(?:as|unchanged|untouched|the\s+same)",
    r"stay(?:s|ed)?\s+(?:as|unchanged|untouched|the\s+same)",
    r"\bis\s+unchanged\b",
    r"\bwas\s+not\s+(?:changed|modified|edited|touched)\b",
    r"\bnot\s+touched\b",
    r"\bno\s+(?:change|modification|edit)s?\s+(?:was|were|is|are)?\s*"
    r"(?:needed|required|necessary)?",
    r"need(?:s|ed)?\s+no\s+(?:change|modification|edit)",
    r"require(?:s|d)?\s+no\s+(?:change|modification|edit)",
    r"unmodified\b",
)

# A CHANGE row may legitimately QUOTE a superseded claim, which is exactly what
# the corrected README row does. These markers, near the hit, mark it as such.
# THE EXCUSE IS A STRUCTURAL SENTINEL, NOT A WORDING HEURISTIC.
#
# A negation is excused only if the line carrying it also carries the literal
# token `[superseded]`. That is it. No marker vocabulary, no proximity window,
# no sentence scoping, no attempt to decide from English whether a correction
# refers to this claim or another one.
#
# WHY, measured across three `claim-check` failures of this phase:
#
#   pass 3 -- the excuse was a 260/160 CHARACTER WINDOW. Broken by
#       "We corrected the heading above. The badge was left alone because
#        it's fine."
#     "corrected" was about the heading; the window did not care.
#
#   pass 4 -- the excuse was SENTENCE SCOPE. Broken by
#       "An earlier version of the OTHER file was corrected; this file was
#        left alone."
#     The marker shares the sentence and refers to a different file. Four such
#     constructions were demonstrated live.
#
# Each fix narrowed the proximity rule and each was defeated by a sentence that
# a human reads as unambiguously false. The lesson is not that the window or
# the sentence was the wrong radius -- it is that **deciding whether a
# correction refers to this claim is reference resolution, and a regex cannot
# do it.** A third tightening would have been the same mistake a third time.
#
# So the author now says so explicitly. `[superseded]` is unambiguous, cannot
# be produced accidentally by prose, and is trivially greppable in review. It
# is still an opt-out, and someone could sprinkle it to silence the check --
# but that is now a deliberate, visible act in a diff rather than an accident
# of phrasing, which is the most a check at this layer can honestly offer.
SUPERSEDED_SENTINEL = "[superseded]"

CHANGE_TOKEN = "**CHANGE**"
ROW_RE = re.compile(r"^\|\s*`([^`]+)`\s*\|.*\|\s*(.+?)\s*\|\s*$")
SECTION_RE = re.compile(r"^###\s+`([^`]+)`\s*$")


def parse_record(text):
    """Return (classifications, rationales) keyed by file path.

    classifications: path -> verdict cell from the summary table
    rationales:      path -> the prose under '### `path`'
    """
    classifications, rationales = {}, {}
    current, buf = None, []
    for line in text.splitlines():
        m = SECTION_RE.match(line)
        if m:
            if current is not None:
                rationales[current] = "\n".join(buf).strip()
            current, buf = m.group(1), []
            continue
        if current is not None:
            if line.startswith("## "):
                rationales[current] = "\n".join(buf).strip()
                current, buf = None, []
            else:
                buf.append(line)
        m = ROW_RE.match(line)
        if m:
            classifications[m.group(1)] = m.group(2)
    if current is not None:
        rationales[current] = "\n".join(buf).strip()
    return classifications, rationales


def _line_bounds(low, start):
    """(start, end) of the line containing offset `start`."""
    ls = low.rfind("\n", 0, start) + 1
    le = low.find("\n", start)
    if le == -1:
        le = len(low)
    return ls, le


def _is_excused(low, start, end):
    """True only if the negation's own LINE carries the literal sentinel.

    Structural, not semantic. See the note on SUPERSEDED_SENTINEL for the two
    proximity-based versions this replaced and the strings that defeated them.
    """
    ls, le = _line_bounds(low, start)
    return SUPERSEDED_SENTINEL in low[ls:le]


def find_contradictions(classifications, rationales):
    """A row marked CHANGE whose prose claims the file was not changed.

    Returns a list of (path, matched_text). Pure, so the test can call it with
    hand-built inputs instead of needing a repository.
    """
    out = []
    for path, verdict in sorted(classifications.items()):
        if CHANGE_TOKEN not in (verdict or ""):
            continue
        why = rationales.get(path) or ""
        low = why.lower()
        for pat in NO_CHANGE_PATTERNS:
            for m in re.finditer(pat, low):
                if not _is_excused(low, m.start(), m.end()):
                    out.append((path, m.group(0).strip()))
                    break
            else:
                continue
            break
    return out


def check_file(path):
    if not os.path.exists(path):
        return ["%s: record not found" % path]
    with io.open(path, encoding="utf-8") as fh:
        text = fh.read()
    classifications, rationales = parse_record(text)
    if not classifications:
        # A record this checker cannot parse must not pass silently: an empty
        # population is structurally unable to fail, which is how a scanner in
        # this repository once read 245 of 3,490 files and reported OK.
        return ["%s: parsed zero judgement rows -- the check would be vacuous"
                % path]
    problems = []
    for p, hit in find_contradictions(classifications, rationales):
        problems.append(
            "%s: row `%s` is marked CHANGE but its rationale says %r -- a "
            "record must not contradict its own classification" % (path, p, hit))
    return problems


def self_test():
    ok = True

    def expect(name, cond):
        nonlocal ok
        print("  [%s] %s" % ("ok  " if cond else "FAIL", name))
        ok = ok and bool(cond)

    cls = {"a.md": CHANGE_TOKEN, "b.md": "no change"}

    expect("a bare CHANGE row with no negation passes",
           not find_contradictions(cls, {"a.md": "Rewrote the broken link."}))
    expect("CHANGE + 'left alone' is caught",
           find_contradictions(cls, {"a.md": "The badge was left alone."}))
    for syn in ("remains as it was", "is unchanged", "kept as is",
                "stays the same", "needs no modification", "was not modified",
                "requires no change", "left untouched"):
        expect("CHANGE + %r is caught" % syn,
               find_contradictions(cls, {"a.md": "The badge %s." % syn}))
    expect("a sentinel-marked superseded claim is excused",
           not find_contradictions(cls, {
               "a.md": "[superseded] it was left alone -- that was false; it "
                       "was replaced."}))
    expect("prose claiming a correction, with NO sentinel, is caught",
           find_contradictions(cls, {
               "a.md": "An earlier version said it was left alone. That was "
                       "false; it was replaced."}))
    expect("the pass-3 window bypass is caught",
           find_contradictions(cls, {
               "a.md": "We corrected the heading above. The badge was left "
                       "alone because it's fine."}))
    expect("the pass-4 same-sentence bypass is caught",
           find_contradictions(cls, {
               "a.md": "An earlier version of the OTHER file was corrected; "
                       "this file was left alone."}))
    expect("a sentinel on a DIFFERENT line does not excuse",
           find_contradictions(cls, {
               "a.md": "[superseded] some other claim.\nThe badge was left "
                       "alone."}))
    expect("a no-change row is not policed for this",
           not find_contradictions(cls, {"b.md": "Left alone entirely."}))
    expect("an unparsable record is reported, not passed",
           check_file(os.devnull))
    print("self-test: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("records", nargs="*", default=list(DEFAULT_RECORDS),
                    help="judgement record(s) to check")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    problems = []
    for rec in (args.records or list(DEFAULT_RECORDS)):
        problems.extend(check_file(rec))
    if problems:
        print("[judgement-prose] FAIL")
        for p in problems:
            print("  %s" % p)
        return 1
    print("[judgement-prose] OK: %d record(s) checked; no row contradicts its "
          "own classification." % len(args.records or DEFAULT_RECORDS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
