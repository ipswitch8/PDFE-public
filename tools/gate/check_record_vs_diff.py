# -*- coding: utf-8 -*-
"""A per-file judgement record must agree with the diff, in both directions.

THE PRIMARY CONTROL FOR THE JUDGEMENT RECORD
--------------------------------------------
`docs/ci/repo-migration-2026-10/phase-5-file-judgements.md` records, per file,
whether that file needed a change. Two things can go wrong, and this checks the
one that does not involve reading prose:

* a file is in the diff with no `**CHANGE**` row — it was modified and nobody
  recorded a judgement for it;
* a row says `**CHANGE**` but the file is not in the diff — the record claims
  work that did not happen.

Unlike `check_judgement_prose.py`, this reads **no prose at all**: it compares
two sets of paths. That is why it is the primary control and the prose checker
is explicitly the second line of defence.

WHY IT IS TRACKED, WHICH IS THE WHOLE POINT
-------------------------------------------
This logic existed as a shell script in a Claude job's temp directory for most
of phase-5, while three documents and `check_judgement_prose.py`'s own
docstring described it as "the primary control". The `claim-check` gate failed
the phase on its fifth pass for exactly that: it searched the tracked tree,
found zero hits, located the real implementation in scratch space, and pointed
out that a script about to be deleted is not a control.

That was the **third** occurrence of this defect in one pipeline — phase-4's
rename preflight was untracked, `check_judgement_prose.py`'s first version was
untracked — and the third one was committed *inside the claim being used to
justify the second one's known weakness*. Diagnosing a shape twice is no
protection against repeating it. Hence: tracked, tested, wired, and a test that
asserts all three.

WHAT THIS DOES **NOT** COVER, SAID BEFORE ANYONE ASSUMES OTHERWISE
------------------------------------------------------------------
The comparison is scoped to the commit that last touched the record (or the
working tree, while the record is dirty). It is therefore a check that **one
phase's record matches that phase's commit** -- not a perpetual ledger of every
subsequent commit. A later, unrelated commit that modifies a file without
updating this record is **not** caught here, and is not meant to be.

That scoping is deliberate, but it is exactly the kind of limit that gets
quietly inflated into "the record is kept honest by a standing check". It is
not. It keeps ONE record honest about ONE commit, and the `claim-check` gate
failed this phase five times for claims broader than their mechanism.

A NOTE ON THE FALSE DISAGREE THIS ONCE PRODUCED
-----------------------------------------------
The scratch version reported DISAGREE on two identical sets, because it
indented the paths for display *inside* the file it then diffed. Formatting
belongs in the output, never in the data under comparison. `normalise()` exists
for that, and a regression test feeds it indented input.
"""
import argparse
import io
import os
import re
import subprocess
import sys

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_RECORD = os.path.join("docs", "ci", "repo-migration-2026-10",
                              "phase-5-file-judgements.md")

# The record documents cannot be judged by themselves: they name the files they
# are about, so counting them makes the phase a candidate for judging itself and
# the population moves every time the record is edited.
DEFAULT_EXCLUDE = r"^docs/ci/repo-migration-2026-10/phase-5"

CHANGE_TOKEN = "**CHANGE**"
ROW_RE = re.compile(r"^\|\s*`([^`]+)`\s*\|.*\|\s*(.+?)\s*\|\s*$")


def normalise(paths):
    """Bare, forward-slashed, de-duplicated paths.

    Strips whitespace because the scratch predecessor of this checker indented
    its paths for display and then compared the indented copy against an
    unindented one, reporting DISAGREE on two identical sets.
    """
    out = set()
    for p in paths:
        p = (p or "").strip().strip('"').replace("\\", "/")
        if p:
            out.add(p)
    return out


def change_rows(record_text):
    """Paths whose summary-table verdict contains CHANGE."""
    rows = []
    for line in record_text.splitlines():
        m = ROW_RE.match(line)
        if m and CHANGE_TOKEN in m.group(2):
            rows.append(m.group(1))
    return normalise(rows)


def all_rows(record_text):
    rows = []
    for line in record_text.splitlines():
        m = ROW_RE.match(line)
        if m:
            rows.append(m.group(1))
    return normalise(rows)


def _git(args, cwd="."):
    return subprocess.run(["git"] + args, cwd=cwd, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")


def working_diff(cwd="."):
    out = _git(["status", "--porcelain"], cwd).stdout
    return normalise(l[3:] for l in out.splitlines())


def record_commit_diff(record, cwd="."):
    """Files changed by the commit that last touched `record`.

    WHY THIS EXISTS -- a defect in the first version of this checker, found
    within a minute of committing it.
    ------------------------------------------------------------------------
    The first version compared only against `git status --porcelain`: the
    WORKING diff. That is right while the work is uncommitted and **wrong the
    instant it is committed**, because the working tree is then clean and every
    `**CHANGE**` row looks like a claim about work that never happened.

    Since this checker is wired into `fast-checks.yml`, which runs on push
    against a clean checkout, the first version was **structurally guaranteed
    to fail on every push**. It reported exactly that, on all seven rows, the
    moment phase-5 was committed.

    This repository's recorded failure mode is a check "structurally unable to
    fail". This was its mirror -- structurally unable to PASS -- and it is
    worse in one way: it would have reddened CI for a reason that has nothing
    to do with the tree being wrong.

    So: if the record itself has uncommitted edits, the working diff is the
    right comparison. Otherwise the record is committed, and the right
    comparison is the commit that introduced it.
    """
    rel = record.replace("\\", "/")
    dirty = _git(["status", "--porcelain", "--", rel], cwd).stdout.strip()
    if dirty:
        return working_diff(cwd), "working tree"
    sha = _git(["log", "-1", "--format=%H", "--", rel], cwd).stdout.strip()
    if not sha:
        # Never committed and not dirty: nothing to compare against. Report it
        # rather than pass -- an empty comparison cannot fail.
        return None, "no commit and no working change"
    out = _git(["show", "--name-only", "--format=", sha], cwd).stdout
    return normalise(out.splitlines()), "commit %s" % sha[:8]


def compare(rows, diffed, exclude=DEFAULT_EXCLUDE):
    """Return (claimed_but_absent, present_but_unjudged). Pure."""
    pat = re.compile(exclude) if exclude else None
    keep = lambda p: not (pat and pat.match(p))
    rows = {p for p in rows if keep(p)}
    diffed = {p for p in diffed if keep(p)}
    return sorted(rows - diffed), sorted(diffed - rows)


def check(record=DEFAULT_RECORD, cwd=".", exclude=DEFAULT_EXCLUDE):
    path = os.path.join(cwd, record) if cwd != "." else record
    if not os.path.exists(path):
        return ["%s: record not found" % record]
    with io.open(path, encoding="utf-8") as fh:
        text = fh.read()
    every = all_rows(text)
    if not every:
        # An empty population is structurally unable to fail. This repository
        # has shipped a scanner that read 245 of 3,490 files and reported OK.
        return ["%s: parsed zero judgement rows -- the comparison would be "
                "vacuous" % record]
    diffed, source = record_commit_diff(record, cwd)
    if diffed is None:
        return ["%s: %s -- nothing to compare the record against" % (record,
                                                                     source)]
    claimed, unjudged = compare(change_rows(text), diffed, exclude)
    problems = []
    for p in claimed:
        problems.append("`%s` is marked CHANGE in the record but is not in the "
                        "diff -- the record claims work that did not happen" % p)
    for p in unjudged:
        problems.append("`%s` is in the diff but has no CHANGE row -- it was "
                        "modified with no recorded judgement" % p)
    return problems


def self_test():
    ok = True

    def expect(name, cond):
        nonlocal ok
        print("  [%s] %s" % ("ok  " if cond else "FAIL", name))
        ok = ok and bool(cond)

    expect("identical sets agree",
           compare({"a", "b"}, {"a", "b"}, exclude="") == ([], []))
    expect("a file in the diff with no row is reported",
           compare({"a"}, {"a", "b"}, exclude="") == ([], ["b"]))
    expect("a CHANGE row with no diff entry is reported",
           compare({"a", "b"}, {"a"}, exclude="") == (["b"], []))
    # The false-DISAGREE regression: indentation must not make sets differ.
    expect("indented input does not fake a disagreement",
           compare(normalise(["  a", "\tb"]), normalise(["a", "b"]),
                   exclude="") == ([], []))
    expect("backslash paths normalise to forward slashes",
           compare(normalise([r"x\y"]), normalise(["x/y"]), exclude="")
           == ([], []))
    expect("the exclusion is applied to both sides",
           compare({"docs/ci/repo-migration-2026-10/phase-5.md"},
                   set()) == ([], []))
    expect("a row parses out of a summary table line",
           change_rows("| `a/b.md` | prose | 1 | **CHANGE** |") == {"a/b.md"})
    expect("a no-change row is not read as CHANGE",
           change_rows("| `a/b.md` | prose | 1 | no change |") == set())
    expect("an empty record is reported, not passed",
           check(record=os.devnull))
    print("self-test: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--record", default=DEFAULT_RECORD)
    ap.add_argument("--exclude", default=DEFAULT_EXCLUDE,
                    help="regex of paths judged by neither side")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    problems = check(record=args.record, exclude=args.exclude)
    if problems:
        print("[record-vs-diff] FAIL")
        for p in problems:
            print("  %s" % p)
        return 1
    # Name the comparison source. A check that does not say what it compared
    # against cannot be audited, and this one has two legitimate sources.
    _, source = record_commit_diff(args.record)
    print("[record-vs-diff] OK: every CHANGE row is present and every changed "
          "file has a CHANGE row (compared against %s)." % source)
    return 0


if __name__ == "__main__":
    sys.exit(main())
