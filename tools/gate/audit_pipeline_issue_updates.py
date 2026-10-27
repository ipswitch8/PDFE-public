#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Did every GitHub issue a pipeline touched actually get updated?

WHY THIS EXISTS
---------------
A pipeline's phase notes say which issues it addressed. Those notes are written
by the same process that did the work, so they are a claim, not evidence. The
thing a reader actually relies on is the ISSUE: its open/closed state, and
whether it says what happened.

Nothing checked that. Pipeline `p83-outstanding` closed with every phase gated
green, and a manual audit afterwards still found four issues in an inconsistent
state:

  * a CLOSED issue (#55) whose fix had been shown not to take effect, with no
    cross-reference to the follow-up
  * an OPEN issue (#131) that a phase had touched and told nothing
  * an OPEN issue (#173) whose latest comment opened with the word "Fixed"
  * two newly-filed issues with no labels, where every comparable issue has them

None of those is catchable by a gate: karen and validator verify the WORK, and
the work was done. This checks the paper trail instead.

WHAT IT CHECKS
--------------
Issue ids are derived from the pipeline plan itself (every ``#NNN`` appearing
anywhere in ``pipeline.json``), so the list cannot drift from the plan the way a
hand-maintained one would.

For each, it reports and flags:

  * **no comment since the pipeline started** -- the issue was in scope and was
    told nothing. Newly-FILED issues are exempt: their body is the record.
  * **CLOSED with no comment** -- closed silently, so the reason lives only in a
    commit message someone would have to go looking for.
  * **OPEN whose last comment opens with a "done" word** -- reads as finished at
    a glance while the state says otherwise. The remainder may well be stated
    further down; the point is that a scanner will not see it.
  * **missing labels** relative to the repository's own convention.

EXIT CODE is 1 if anything is flagged, so this can gate. It is advisory by
design though: every flag is a prompt to look, not proof of a defect.

USAGE
-----
    python tools/gate/audit_pipeline_issue_updates.py \\
        --pipeline .claude/pipelines/p83-outstanding/pipeline.json \\
        --since 2026-09-08 \\
        --filed 181,182

Requires the `gh` CLI, authenticated. Reaches GitHub read-only; changes nothing.
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

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

#: `gh` is not always on PATH for a child process on Windows even when the
#: shell can see it -- it lives at ~/bin/gh.cmd here. Resolve explicitly rather
#: than letting subprocess fail with a bare FileNotFoundError.
GH = (shutil.which("gh")
      or shutil.which("gh.cmd")
      or os.path.expanduser(os.path.join("~", "bin", "gh.cmd")))

#: Words that make a comment read as "this is finished" at a glance.
DONE_OPENERS = ("fixed", "done", "resolved", "complete", "completed", "closing")


def gh_json(args):
    if not GH or not os.path.exists(GH) and not shutil.which("gh"):
        raise SystemExit("audit: the gh CLI was not found; cannot query GitHub")
    p = subprocess.run([GH] + args, capture_output=True, text=True)
    if p.returncode != 0:
        return None
    try:
        return json.loads(p.stdout)
    except ValueError:
        return None


def issue_ids_from_plan(path: str):
    """Every #NNN mentioned anywhere in the plan, in ascending order."""
    with io.open(path, encoding="utf-8") as fh:
        blob = fh.read()
    return sorted({int(m) for m in re.findall(r"#(\d+)", blob)})


def judge(state, recent, labels, filed, since, require_labels):
    """The per-issue verdict, factored out so --self-test can exercise it.

    Returns a list of problem strings; empty means clean.
    """
    problems = []
    if not recent and not filed:
        problems.append("no comment since %s" % since)
    if state == "CLOSED" and not recent:
        problems.append("closed with no comment")
    if state == "OPEN" and recent:
        first_line = (recent[-1].get("body") or "").strip().lstrip("*_# ").lower()
        opener = first_line.split()[0].strip("*_:,.—-") if first_line else ""
        if opener in DONE_OPENERS:
            problems.append("OPEN but last comment opens '%s'" % opener)
    for want in require_labels:
        if want not in labels:
            problems.append("missing label '%s'" % want)
    return problems


def self_test() -> int:
    """Prove each check can FIRE and can stay QUIET. A guard that has only ever
    passed is indistinguishable from one that cannot fail."""
    cases = [
        # (name, args..., expect_substring_or_None)
        ("silent closed issue",
         dict(state="CLOSED", recent=[], labels=set(), filed=False),
         "closed with no comment"),
        ("in-scope issue told nothing",
         dict(state="OPEN", recent=[], labels=set(), filed=False),
         "no comment since"),
        ("newly filed issue is exempt",
         dict(state="OPEN", recent=[], labels=set(), filed=True),
         None),
        ("OPEN whose last comment says Fixed",
         dict(state="OPEN", recent=[{"body": "Fixed — the thing works now."}],
              labels=set(), filed=False),
         "opens 'fixed'"),
        ("OPEN whose last comment is a status note",
         dict(state="OPEN", recent=[{"body": "Status clarification, because..."}],
              labels=set(), filed=False),
         None),
        ("bold markdown opener is still detected",
         dict(state="OPEN", recent=[{"body": "**Done.** shipped in abc123"}],
              labels=set(), filed=False),
         "opens 'done'"),
        ("CLOSED with a comment is fine",
         dict(state="CLOSED", recent=[{"body": "Fixed and closing."}],
              labels=set(), filed=False),
         None),
    ]
    failures = 0
    for name, kw, expect in cases:
        got = judge(since="2026-01-01", require_labels=[], **kw)
        joined = "; ".join(got)
        if expect is None:
            ok = not got
        else:
            ok = expect in joined
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               "" if ok else "  -> got %r" % joined))
        if not ok:
            failures += 1

    # Label check fires and stays quiet.
    got = judge("OPEN", [{"body": "note"}], set(), False, "2026-01-01", ["gap-sec"])
    ok = "missing label 'gap-sec'" in "; ".join(got)
    print("  [%s] missing label is reported" % ("PASS" if ok else "FAIL"))
    failures += 0 if ok else 1
    got = judge("OPEN", [{"body": "note"}], {"gap-sec"}, False, "2026-01-01", ["gap-sec"])
    ok = not got
    print("  [%s] present label is not reported" % ("PASS" if ok else "FAIL"))
    failures += 0 if ok else 1

    total = len(cases) + 2
    print("self-test: %d/%d checks passed" % (total - failures, total))
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="exercise each check against synthetic records "
                         "and exit; needs no network and no gh")
    ap.add_argument("--pipeline",
                    help="path to the pipeline.json whose issues to audit")
    ap.add_argument("--since",
                    help="ISO date the pipeline started; comments before this "
                         "do not count as 'this pipeline told the issue'")
    ap.add_argument("--filed", default="",
                    help="comma-separated ids FILED by this pipeline; their "
                         "body is the record, so they are exempt from the "
                         "no-comment check")
    ap.add_argument("--require-label", action="append", default=[],
                    help="a label every audited issue should carry; repeatable")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.pipeline or not args.since:
        ap.error("--pipeline and --since are required unless --self-test")

    filed = {int(x) for x in args.filed.split(",") if x.strip()}
    ids = sorted(set(issue_ids_from_plan(args.pipeline)) | filed)
    if not ids:
        print("audit: the plan references no issue ids -- nothing to check, "
              "which is itself suspicious for a backlog pipeline")
        return 1

    print("auditing %d issue(s) from %s" % (len(ids), args.pipeline))
    print("%-6s %-7s %-8s %-9s %s" % ("#", "state", "comments", "since", "flags"))
    print("-" * 78)

    flagged = []
    for n in ids:
        d = gh_json(["issue", "view", str(n), "--json",
                     "number,state,title,comments,labels"])
        if d is None:
            print("%-6s QUERY FAILED" % n)
            flagged.append((n, "gh query failed"))
            continue

        comments = d.get("comments") or []
        recent = [c for c in comments if (c.get("createdAt") or "") >= args.since]
        labels = {l["name"] for l in (d.get("labels") or [])}
        state = d["state"]
        problems = judge(state, recent, labels, n in filed, args.since,
                         args.require_label)

        print("%-6s %-7s %-8s %-9s %s" %
              (n, state, len(comments), len(recent),
               "; ".join(problems) if problems else "ok"))
        if problems:
            flagged.append((n, "; ".join(problems)))

    print()
    if not flagged:
        print("audit: OK -- every issue this pipeline touched carries a comment "
              "from it, and no state/prose mismatch was found.")
        return 0

    print("audit: %d issue(s) need attention:" % len(flagged))
    for n, why in flagged:
        print("   #%-5s %s" % (n, why))
    print()
    print("Each flag is a prompt to LOOK, not proof of a defect -- an OPEN issue "
          "whose comment opens with 'Fixed' may well state the remainder further "
          "down. The point is that a reader scanning state will not see it.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
