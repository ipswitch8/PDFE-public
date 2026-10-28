#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_publication_block.py — the repository must not go public while
unreferenced objects carrying removed identity data are still served by GitHub.

WHY THIS EXISTS.

pub-readiness phase-3 measured that **251 of 251** commits orphaned by the
2026-10-18 history rewrite are still served by SHA, and that **28** of them
still expose the identity the rewrite removed — through author/committer
fields, commit messages, and the diff content the commits API returns. The SHAs
are not secret: every Actions run records the `head_sha` it ran against, so
they can be read straight out of the Actions API.

The phase's conclusion — *the repository must stay private until GitHub
confirms the objects are collected* — was written in three documents and
enforced by nothing. The `claim-check` gate failed the phase for exactly that:
it is the single highest-consequence claim in the pipeline, its failure mode is
**irreversible** (publishing cannot be undone), and no branch rule, workflow or
check would have stopped, or even noticed, a flip to public.

This is the mechanism. It cannot physically prevent a visibility change — only
GitHub's own settings can — but it makes the condition machine-checked and
loud, so the violation surfaces instead of passing silently.

WHAT IT CHECKS

1. If the repository is PUBLIC while the garbage-collection request is not yet
   confirmed COLLECTED, fail. This is the irreversible case.
2. If the request's status claims SUBMITTED or COLLECTED without recording a
   Support ticket reference, fail. That is the staleness direction the same
   gate flagged separately: a status line a human must remember to update can
   drift in either direction, and "claims submitted with no evidence" is the
   half a machine can detect.
3. If the status marker is missing or unrecognised, fail — the document must
   stay machine-readable, or this guard silently stops guarding.

FAIL-CLOSED ON UNKNOWN. If repository visibility cannot be determined (no `gh`,
no auth, no network), this exits 2 rather than 0. A guard whose failure mode is
"assume private" would be worthless precisely when it matters. Exit 2 is
distinguished from exit 1 so a local offline run is legible as "could not
check", not misread as "violation found".

The decision logic is a pure function (`decide`) with no I/O, so it is directly
unit-testable — see tools/gate/test_check_publication_block.py.

Usage:
    python tools/gate/check_publication_block.py
    python tools/gate/check_publication_block.py --assume-private    # offline
    python tools/gate/check_publication_block.py --self-test

Exit 0 = compliant; 1 = violation or malformed status; 2 = could not determine.
"""
from __future__ import annotations

import argparse
import io
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GC_REQUEST = REPO_ROOT / "docs" / "legal" / "github-gc-request-2026-10.md"
REPO_SLUG = "ipswitch8/PDFE"

# EVERY repository that serves the orphaned pre-rewrite objects, not just the
# one this guard was originally written for.
#
# Added after the `claim-check` and `security-audit` gates independently found
# that this script checked `ipswitch8/PDFE` alone, so the 2026-10 migration's
# two new repositories were unguarded: their privacy was a hand-verified
# snapshot, and nothing automated would have noticed a flip. One of them is the
# abandoned first attempt, which **received a full history push** before being
# set aside, so it serves the same objects as the original.
#
# A renamed repository still resolves through GitHub's redirect, so entries
# survive the phase-4 renames; an entry that 404s is reported as absent and is
# NOT a violation (a repository that no longer exists cannot be public).
#
# The redirect claim is MEASURED, not assumed: `PDFE-migrating` was renamed to
# `PDFE-migrating-abandoned` during phase 1, and `gh api repos/ipswitch8/
# PDFE-migrating` still returns 200 with `name: PDFE-migrating-abandoned` and
# the same id (1427598740). So a stale slug here keeps guarding the repository
# it used to name rather than silently becoming a no-op.
#
# The corollary matters more: a redirect means a slug can start pointing at a
# DIFFERENT repository than the one intended. Between the two phase-4 renames
# there is a window where `ipswitch8/PDFE` resolves to the RETIRED repository,
# and once that rename frees `PDFE-migration`, anything later taking that name
# would be guarded in place of the repository the entry meant.
#
# So identity is asserted by NUMERIC ID -- see GUARDED_REPO_IDS below, which is
# the authoritative set, with these names kept only for the candidate decision
# and for human-readable output.
#
# An earlier version of this comment claimed identity "is only ever asserted by
# numeric id elsewhere in the pipeline". The `claim-check` gate grepped for that
# mechanism and found it did not exist anywhere -- a claim describing a safety
# net that had been imagined rather than built, in the file gating the one
# irreversible action here. The id check below is that sentence made true; it
# was written because the claim was found false, not before.
#
# CURRENT AS OF PHASE-4, WHICH RENAMED TWO OF THESE PAST EACH OTHER. Before
# that rename this tuple read:
#
#     "ipswitch8/PDFE",            # the original; serves all 287 orphans
#     "ipswitch8/PDFE-migration",  # the clean replacement
#     "ipswitch8/PDFE_old2",       # the name the source TAKES in phase-4
#
# and every one of those comments became false the moment the rename landed:
# `ipswitch8/PDFE` is now the REPLACEMENT, `PDFE-migration` is a redirect to
# that SAME repository (so two of four entries named one repo), `PDFE_old2` is
# no longer a future tense, and "all 287 orphans" was separately corrected to
# 282 unreferenced plus 5 referenced by live dependabot branch tips.
#
# `PDFE-migration` was DROPPED rather than corrected, on the reasoning in the
# paragraph above: it is now a redirect to the replacement, which is already
# listed under its real name, and the freed name is available for anyone to
# take. Keeping it would mean guarding a stranger's repository in place of the
# one the entry meant -- the exact hazard that paragraph describes, which is an
# argument against retaining a freed slug, not for it.
#
# The id tuple below followed the rename with no edit; this one could not, which
# is the whole argument for the split.
#
# NOT READ AT RUNTIME. Measured by AST-walking this file for reads of the name:
# there is exactly one reference, the assignment on the next line. `main()`
# iterates GUARDED_REPO_IDS; the candidate decision uses PUBLICATION_CANDIDATE.
# Every other mention of GUARDED_SLUGS in this repository is a comment or a test
# assertion, which means five assertions in test_check_publication_block.py
# describe a tuple no guard consults -- they cannot fail in a way that reflects
# guard behaviour. Recorded rather than quietly deleted: the tuple documents
# which repositories the id list is *meant* to be, and that is worth keeping as
# prose, but it must not be mistaken for a control. Flagged for phase-8.
# UPDATED 2026-10-24, after the owner DELETED two repositories:
# `PDFE-migrating-abandoned` and `PDFE_old`. `PDFE-migrating-abandoned` is
# dropped from this tuple rather than annotated, on the precedent set for
# `PDFE-migration` two paragraphs above: a deleted repository frees its name for
# anyone to take, so a slug entry for it would eventually guard a stranger's
# repository in place of the one the entry meant. That is an argument against
# retaining a freed slug, not for it.
#
# The IDS go the other way -- see GUARDED_REPO_IDS below, where both deleted
# repositories are RETAINED. An id is never reassigned, and GitHub can RESTORE a
# deleted repository under its original id, so an id entry for a deleted repo is
# a live control rather than a historical note.
#
# `PDFE_old` (id 1151200494) was never in this tuple at all. That was the gap:
# the pre-first-scrub original -- the repository
# `docs/legal/publication-route-evaluation-2026-10.md` said "should be assumed
# to contain the identity data in full, not merely in orphans" -- was the one
# repository no control watched. It is now deleted and guarded by id.
GUARDED_SLUGS = (
    "ipswitch8/PDFE",                     # the route-B REPLACEMENT (id 1433896232)
    "ipswitch8/PDFE_old3",                # the 2026-10 replacement (id 1427819119);
                                          # host name in reachable history
    "ipswitch8/PDFE_old2",                # the retired ORIGINAL (id 1343217722);
                                          # 282 unreferenced orphans + dirty text
)

# THE AUTHORITATIVE SET: the same repositories, addressed by IMMUTABLE ID.
#
# `GET /repositories/{id}` is name-independent, so these entries survive every
# rename with no list edit and no unguarded window. GitHub does not reassign
# ids, so a 404 here means the repository is genuinely gone -- a much stronger
# statement than a name 404, which could merely mean "renamed".
#
# Ids measured 2026-10-21 and asserted by
# test_check_publication_block.py::GuardedByImmutableId.
# A DELETED repository stays on this list, deliberately. Two reasons, and the
# second is the load-bearing one:
#
#   1. `GET /repositories/{id}` on a deleted repo returns 404, which maps to
#      ABSENT, which `worst_visibility` treats as benign -- so the entry costs
#      one query and can never go red on its own.
#   2. **GitHub can RESTORE a deleted repository, under its original id.** So
#      "deleted" is not a terminal state, and an id entry is the only thing that
#      would notice a restored repository being made public. Dropping the entry
#      because the repo is gone would remove the control exactly when the repo
#      stops being observable by any other means.
#
# This is why the deletions were handled asymmetrically: freed SLUGS are dropped
# (they would come to name a stranger's repo), retired IDS are kept (they can
# only ever name the same repo).
GUARDED_REPO_IDS = (
    (1343217722, "the retired original, now named PDFE_old2; 282 unreferenced "
                 "orphans + dirty issue text, so it stays private permanently"),
    (1427819119, "the 2026-10 replacement, renamed PDFE_old3 on 2026-10-25 "
                 "(p101 route B): its REACHABLE history still names the build "
                 "host (1 commit message + 2 historical blob versions), which "
                 "is why it was migrated again rather than published. Stays "
                 "private"),
    (1433482788, "the FIRST route-B fresh instance, burned within 40 minutes: "
                 "Dependabot fired from the pushed dependabot.yml and consumed "
                 "PR numbers 1-5, making tracker numbering 1..324 impossible "
                 "(numbers are never reused). Renamed PDFE-burned-numbering "
                 "2026-10-25, DELETED by the owner 2026-10-26 (confirmed 404 "
                 "by id). Its content was rewritten-clean history only; the id "
                 "stays pinned because a GitHub restore would reuse it"),
    (1433896232, "the route-B replacement (second instance), holds the PDFE "
                 "name and the 324-issue tracker. As of 2026-10-27 this is the "
                 "private DEV repo under the two-repo publish-snapshot model "
                 "(docs/legal/publication-model-2026-10.md): it STAYS PRIVATE "
                 "permanently -- it carries migration commits and 2 served "
                 "account-name orphans from a remediation force-push, which "
                 "never reach the public repo because the public repo gets "
                 "clean SNAPSHOTS, not this history. It is NOT a publication "
                 "candidate any more; the candidate is ipswitch8/PDFE-public "
                 "(id 1435376025), deliberately NOT in this stay-private set "
                 "because it is MEANT to go public, clean by construction via "
                 "tools/ops/publish_snapshot.py's scan gate"),
    (1427598740, "burned first attempt; received a full history push. DELETED "
                 "by the owner 2026-10-24; retained because a restore would "
                 "reuse this id"),
    (1151200494, "the PRE-FIRST-SCRUB original, named PDFE_old after the "
                 "2026-08-22 migration -- the dirtiest of the set, holding the "
                 "identity data in full rather than only in orphans, and the "
                 "one repository no control watched until now. DELETED by the "
                 "owner 2026-10-24 after its 3 unique artifacts (PRs #1-#3, "
                 "the refs the 2026-08-22 scrub could not remove) were judged "
                 "not worth preserving and its release assets were confirmed "
                 "byte-identical to copies held elsewhere. Retained because a "
                 "restore would reuse this id"),
)

# The repository whose GitHub-side TEXT (issue bodies, comments, release notes)
# must be clean. This stays the DEV repo, which holds the 324-issue tracker --
# the tracker is what has GitHub-side text; the public snapshot repo
# (ipswitch8/PDFE-public) is code-only and has none to scan.
#
# TWO-REPO MODEL NOTE (2026-10-27): the repository that actually goes PUBLIC is
# ipswitch8/PDFE-public, and its safety does NOT come from this marker -- it
# comes from tools/ops/publish_snapshot.py, which scans every file of the
# snapshot against the full needle inventory and refuses to push on any hit,
# and from the fact that it carries no dev history to leak. See
# docs/legal/publication-model-2026-10.md. This check continues to police the
# dev repo's tracker text (kept clean), which is cheap and catches a dirty
# issue body before it could ever be snapshotted into a public release note.
#
# This is deliberately NOT all of GUARDED_SLUGS. The retired original is meant
# to stay private AND dirty -- its issue text carries scrubbed identifiers (6
# hits across 3 bodies, 6 more across 2 issues' comments, measured 2026-10-21)
# and nothing will ever clean it. Demanding cleanliness of every guarded slug
# would make this guard permanently red, which is how a guard becomes noise.
PUBLICATION_CANDIDATE = "ipswitch8/PDFE"

# The live scanner for that text. Issue bodies, comments and release notes live
# in GitHub's DATABASE, not in git, so no scrub guard had ever covered them --
# `check_scrub_regressions.py` (files, blobs, commit messages) and
# `check_tracked_path_hygiene.py` (committable files) are both blind to it, and
# four history rewrites could not have touched it.
#
# Wired in here, rather than left as a script someone remembers to run, because
# the `claim-check` gate failed phase-3 for exactly that: the migrated tracker
# was clean *today* and guarded by nothing tomorrow, while a publication
# decision sat directly downstream of the claim. A doc-only fix would have
# reproduced the defect shape the gate exists to catch.
TRACKER_SCANNER = ("scripts", "migration", "scan_tracker_text.py")

# query_visibility_detailed outcomes.
VIS_PRIVATE = "private"
VIS_PUBLIC = "public"
VIS_ABSENT = "absent"      # no such repository -- cannot be public
VIS_UNKNOWN = "unknown"    # no gh / no auth / no network -- must fail closed

# The machine-readable marker the document carries. Parsed rather than prose,
# because prose is what failed here the first time.
MARKER_RE = re.compile(
    r"<!--\s*publication-block:\s*status=(?P<status>[A-Z]+)\s+ticket=(?P<ticket>\S+)\s*-->"
)

# SUPERSEDED was added 2026-10-26 (p101 phase-6) for the route-B outcome: the
# publication candidate is now a repository that NEVER CONTAINED the orphans
# this request concerns -- the orphan-bearing repositories (old2, old3, the
# burned instance) are pinned private-permanently by id above, and publication
# of the candidate serves none of their bytes (independent object stores,
# fork:false / network_count:0, measured). Demanding a COLLECTED ticket about
# a permanently-private archive before the clean repository may publish gated
# the wrong thing.
#
# SUPERSEDED still requires a RECORDED reason in the ticket field (reuse of the
# existing staleness mechanism: `ticket=<reason-ref>`, never `none`), so the
# flip stays behind a deliberate, machine-checked marker update -- the owner
# decision has to be written down where this guard reads it, exactly as D3
# step 2 of the checklist demanded.
VALID_STATUSES = ("PREPARED", "SUBMITTED", "COLLECTED", "SUPERSEDED")

OK, VIOLATION, UNKNOWN = 0, 1, 2


def decide(status: str | None, ticket: str | None, is_private: bool | None):
    """Pure decision. Returns (exit_code, message).

    `is_private` is None when visibility could not be determined.
    `status`/`ticket` are None when the marker is absent or unparseable.
    """
    if status is None:
        return VIOLATION, (
            "no machine-readable publication-block marker found in "
            f"{GC_REQUEST.relative_to(REPO_ROOT).as_posix()}. Without it this "
            "guard cannot tell whether publication is still blocked, so it "
            "fails rather than assume it is safe."
        )

    if status not in VALID_STATUSES:
        return VIOLATION, (
            f"unrecognised publication-block status {status!r}; expected one of "
            f"{', '.join(VALID_STATUSES)}."
        )

    has_ticket = bool(ticket) and ticket.lower() not in ("none", "n/a", "tbd", "")

    # Staleness, the direction a machine can actually detect: a status that
    # claims progress without recording the evidence for it. SUPERSEDED is held
    # to the same standard -- its "ticket" field must carry a reference to the
    # recorded decision (e.g. route-b-p101), never none: a supersession nobody
    # wrote down is indistinguishable from a bypass.
    if status in ("SUBMITTED", "COLLECTED", "SUPERSEDED") and not has_ticket:
        return VIOLATION, (
            f"status is {status} but no reference is recorded "
            "(ticket=none). A status line that claims progress or supersession, "
            "with nothing identifying the basis, is exactly the silent drift "
            "this guard exists to catch. Record the reference in the marker."
        )

    if is_private is None:
        return UNKNOWN, (
            "could not determine repository visibility (no gh/auth/network). "
            "Reporting UNKNOWN rather than passing: a guard that assumes "
            "private when it cannot check is worthless when it matters. "
            "Re-run with network, or pass --assume-private only if you have "
            "confirmed visibility by another means."
        )

    if is_private:
        return OK, (
            f"repository is private; publication-block status is {status}"
            f"{' (ticket ' + ticket + ')' if has_ticket else ''}. Compliant."
        )

    # Public. Acceptable once GitHub has confirmed collection -- or once the
    # request is recorded SUPERSEDED with its basis (route B: the candidate
    # never contained the orphans; the archives stay private by id, which the
    # per-id loop in main() enforces independently of this marker).
    if status == "COLLECTED":
        return OK, (
            "repository is public and the garbage-collection request is "
            f"recorded COLLECTED (ticket {ticket}). Compliant."
        )
    if status == "SUPERSEDED":
        return OK, (
            "repository is public and the garbage-collection request is "
            f"recorded SUPERSEDED ({ticket}): the publication candidate never "
            "contained the orphans the request concerns, and the orphan-bearing "
            "archives are pinned private by immutable id. Compliant."
        )

    return VIOLATION, (
        "REPOSITORY IS PUBLIC while the garbage-collection request status is "
        f"{status}.\n"
        "    287 commits orphaned by four history rewrites are still served by "
        "SHA, and 28 of them still expose the identity those rewrites removed. "
        "Their SHAs are discoverable from the Actions API. (The figure was 251 "
        "when this guard was written after the first rewrite; it has only ever "
        "grown -- 251, then 279, then 287 -- because nothing has been "
        "collected.)\n"
        "    At least 5 of the 287 are NOT unreferenced: they are the live "
        "HEADs of dependabot branches that still exist, so ordinary GC cannot "
        "collect them until those branches are deleted.\n"
        "    Making this repository public before GitHub confirms collection "
        "publishes those commits, and that cannot be undone. Either make the "
        "repository private again, or — if GitHub has confirmed collection — "
        "update the marker to status=COLLECTED with the ticket reference.\n"
        f"    See {GC_REQUEST.relative_to(REPO_ROOT).as_posix()}."
    )


def read_marker(path: Path):
    """(status, ticket) from the document, or (None, None)."""
    try:
        text = io.open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None, None
    m = MARKER_RE.search(text)
    if not m:
        return None, None
    return m.group("status"), m.group("ticket")


def query_visibility(slug: str):
    """True=private, False=public, None=could not determine."""
    try:
        out = subprocess.run(
            ["gh", "api", f"repos/{slug}", "--jq", ".private"],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    v = (out.stdout or "").strip().lower()
    if v == "true":
        return True
    if v == "false":
        return False
    return None


def query_repo_by_id(repo_id: int):
    """Visibility and current name of a repository, addressed by immutable id.

    Returns (visibility, full_name_or_None). `GET /repositories/{id}` is
    name-independent, so this survives every rename without the list being
    edited -- which is the point. A 404 means the repository is genuinely gone
    (ids are never reassigned), so ABSENT here is a much stronger statement
    than ABSENT from a name lookup, where it could just mean "renamed".
    """
    try:
        out = subprocess.run(
            ["gh", "api", f"repositories/{repo_id}", "--jq",
             "[(.private|tostring), .full_name] | join(\" \")"],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return VIS_UNKNOWN, None
    if out.returncode != 0:
        blob = ((out.stderr or "") + (out.stdout or "")).lower()
        if "404" in blob or "not found" in blob:
            return VIS_ABSENT, None
        return VIS_UNKNOWN, None
    parts = (out.stdout or "").strip().split(" ", 1)
    if len(parts) != 2:
        return VIS_UNKNOWN, None
    priv, name = parts[0].strip().lower(), parts[1].strip()
    if priv == "true":
        return VIS_PRIVATE, name
    if priv == "false":
        return VIS_PUBLIC, name
    return VIS_UNKNOWN, name


def query_visibility_detailed(slug: str) -> str:
    """One of VIS_PRIVATE / VIS_PUBLIC / VIS_ABSENT / VIS_UNKNOWN.

    `query_visibility` collapses "no such repository" and "could not reach
    GitHub" into the same None, which is fine for one repository that is known
    to exist but wrong across a list: a slug that 404s because it was renamed
    away cannot be public, while a slug that fails because there is no auth
    genuinely is unknown and must fail closed. Those two must not share a
    verdict.
    """
    try:
        out = subprocess.run(
            ["gh", "api", f"repos/{slug}", "--jq", ".private"],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return VIS_UNKNOWN
    if out.returncode != 0:
        blob = ((out.stderr or "") + (out.stdout or "")).lower()
        # Distinguish a genuine 404 from auth/network trouble. "not found" with
        # a 404 is the repository being gone; anything else is unknown.
        if "404" in blob or "not found" in blob:
            return VIS_ABSENT
        return VIS_UNKNOWN
    v = (out.stdout or "").strip().lower()
    if v == "true":
        return VIS_PRIVATE
    if v == "false":
        return VIS_PUBLIC
    return VIS_UNKNOWN


def scan_tracker_text(slug: str):
    """Is `slug`'s GitHub-side text free of scrubbed identifiers?

    Returns (clean, detail): clean is True/False, or None when the scan could
    not run at all. None must never be treated as clean -- see `decide_text`.
    """
    script = REPO_ROOT.joinpath(*TRACKER_SCANNER)
    if not script.exists():
        return None, f"{'/'.join(TRACKER_SCANNER)} is missing"
    try:
        out = subprocess.run(
            [sys.executable, str(script), "--source", "live",
             "--repo", slug, "--comments"],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=900,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return None, f"could not run the scanner: {exc}"
    # The scanner exits 0 clean, 1 on hits, 2 when it could not scan -- and it
    # deliberately refuses to report clean on a scan that did not happen.
    if out.returncode == 0:
        return True, "no scrubbed identifier in issue text, comments or release notes"
    if out.returncode == 1:
        first = next((l for l in (out.stdout or "").splitlines()
                      if l.startswith("FAIL:")), "hits found")
        return False, first
    return None, ((out.stdout or "") + (out.stderr or "")).strip()[:200] \
        or "scanner could not complete"


def slugs_requiring_clean_text(status, results):
    """Which slugs must have clean GitHub-side text? Returns (slugs, sanctioned).

    Derived from the OBSERVED visibility, not from a hardcoded name. That
    coupling is the whole point -- see `decide_text`.
    """
    public = sorted({s for s, v in results if v == VIS_PUBLIC})
    if public:
        # Whatever is actually public must be clean, whichever name it has.
        return public, True
    if status == "COLLECTED":
        # Publication is sanctioned but nothing is public yet; the candidate is
        # the thing about to be published.
        return [PUBLICATION_CANDIDATE], True
    return [], False


def decide_text(status, results, clean_by_slug):
    """Should dirty GitHub-side text block, given where publication stands?

    Returns (blocking, note).

    WHY THIS TAKES `results` RATHER THAN ONE SLUG'S VERDICT. The first version
    took `(status, is_private, text_clean)` and scanned only
    `PUBLICATION_CANDIDATE`, on the comment that "the guard follows the name".
    The `claim-check` gate disproved that by invoking the real functions:
    visibility aggregation spanned every guarded slug while the text check was
    bound to a fixed name, and NOTHING COUPLED THE TWO. With
    `PDFE-migration` public and the candidate private-and-clean, the git half
    passed, the text half reported clean about a repository that was not the
    public one, and the guard exited 0 -- a publishable, actually-public
    repository whose text had never been scanned. Reachable by one operator
    sequencing mistake, and invisible to any test of the old shape.

    So the input is now derived from observed visibility. Two independent
    conditions:

      1. INVARIANT -- only `PUBLICATION_CANDIDATE` may ever be public. Any
         other guarded slug going public blocks outright, whatever its text
         says. This turns "the guard follows the name" from a comment into
         something checked.
      2. Whatever is public (or, if nothing is public yet but the marker says
         COLLECTED, the candidate) must have clean text. UNKNOWN counts as
         not-clean, the same fail-closed rule this file applies to visibility.

    While everything is private and the marker is not COLLECTED, dirty text is
    reported but does not block: publication is not happening, and the RETIRED
    repository is meant to stay private and dirty forever, so blocking on that
    would make the guard permanently red.
    """
    rogue = sorted({s for s, v in results
                    if v == VIS_PUBLIC and s != PUBLICATION_CANDIDATE})
    if rogue:
        return True, (
            "a guarded repository other than the publication candidate is "
            f"PUBLIC: {', '.join(rogue)}. Only {PUBLICATION_CANDIDATE} may "
            "ever be public, because that is the only name whose content was "
            "migrated clean; the others still serve orphaned pre-rewrite "
            "objects and, in the retired repository's case, identifiers in "
            "issue text."
        )

    required, sanctioned = slugs_requiring_clean_text(status, results)

    if not sanctioned:
        dirty = [s for s in clean_by_slug if clean_by_slug[s] is False]
        if dirty:
            return False, ("tracker text has scrubbed identifiers in "
                           f"{', '.join(sorted(dirty))} -- NOT blocking while "
                           "everything is private, but this WILL block "
                           "publication")
        unknown = [s for s in clean_by_slug if clean_by_slug[s] is None]
        if unknown:
            return False, ("tracker text could not be scanned for "
                           f"{', '.join(sorted(unknown))} -- not blocking "
                           "while private, but publication will require it")
        return False, "tracker text is clean (not enforced while private)"

    bad = [s for s in required if clean_by_slug.get(s) is not True]
    if not bad:
        return False, f"tracker text is clean for {', '.join(required)}"
    unscannable = [s for s in bad if clean_by_slug.get(s) is None]
    if unscannable:
        return True, ("publication is sanctioned but the tracker text could "
                      f"not be scanned for {', '.join(sorted(unscannable))}; "
                      "refusing to assume it is clean")
    return True, ("publication is sanctioned but scrubbed identifiers remain "
                  f"in the issue text, comments or release notes of "
                  f"{', '.join(sorted(bad))}")


def worst_visibility(results):
    """Collapse per-repo visibility into the single value `decide` needs.

    Order of severity, worst first: any PUBLIC repository is a violation
    outright; otherwise any UNKNOWN means we cannot claim the condition holds;
    otherwise every repository is private or absent, which satisfies it.

    `results` is an iterable of (slug, visibility).
    """
    seen = list(results)
    if any(v == VIS_PUBLIC for _, v in seen):
        return False            # -> decide() reports VIOLATION
    if any(v == VIS_UNKNOWN for _, v in seen):
        return None             # -> decide() reports UNKNOWN (fails closed)
    return True                 # all private or absent


def self_test() -> int:
    """Both directions, including the ones that must FAIL."""
    cases = [
        # (status, ticket, is_private, expected_exit, label)
        ("PREPARED", "none", True, OK, "private + prepared is the normal state"),
        ("PREPARED", "none", False, VIOLATION, "public before collection is the irreversible case"),
        ("SUBMITTED", "none", True, VIOLATION, "submitted with no ticket is unevidenced"),
        ("SUBMITTED", "1234567", True, OK, "submitted with a ticket is fine while private"),
        ("SUBMITTED", "1234567", False, VIOLATION, "submitted is not collected; still blocked"),
        ("COLLECTED", "1234567", False, OK, "public is allowed once collection is confirmed"),
        ("COLLECTED", "none", False, VIOLATION, "collected with no ticket is unevidenced"),
        ("BOGUS", "none", True, VIOLATION, "unrecognised status must not pass"),
        (None, None, True, VIOLATION, "a missing marker must not pass"),
        ("PREPARED", "none", None, UNKNOWN, "unknown visibility fails closed, distinctly"),
        # SUPERSEDED (route B): the candidate never held the orphans. The
        # supersession must still be RECORDED -- a reasonless one is refused in
        # both visibility states, so it cannot function as a bypass.
        ("SUPERSEDED", "route-b-p101", True, OK,
         "superseded with a recorded basis is fine while private"),
        ("SUPERSEDED", "route-b-p101", False, OK,
         "public is allowed once the request is recorded superseded with basis"),
        ("SUPERSEDED", "none", True, VIOLATION,
         "superseded with no recorded basis is a bypass, not a decision"),
        ("SUPERSEDED", "none", False, VIOLATION,
         "public + reasonless supersession is the worst case and must fail"),
    ]
    bad = 0
    for status, ticket, priv, want, label in cases:
        got, _ = decide(status, ticket, priv)
        mark = "ok  " if got == want else "FAIL"
        if got != want:
            bad += 1
        print(f"  [{mark}] {label} (want {want}, got {got})")

    # worst_visibility: the aggregation across several repositories. A guard
    # that checked one repository and reported on all of them is exactly the
    # defect two gates caught, so the collapse rule is tested rather than
    # assumed -- including that ABSENT is benign and UNKNOWN is not.
    agg_cases = [
        ([("a", VIS_PRIVATE)], True, "one private repo satisfies it"),
        ([("a", VIS_PRIVATE), ("b", VIS_PRIVATE)], True, "all private satisfies it"),
        ([("a", VIS_PRIVATE), ("b", VIS_ABSENT)], True,
         "absent is benign -- a repo that does not exist cannot be public"),
        ([("a", VIS_ABSENT), ("b", VIS_ABSENT)], True, "all absent satisfies it"),
        ([("a", VIS_PRIVATE), ("b", VIS_PUBLIC)], False,
         "ONE public repo is a violation even if the others are private"),
        ([("a", VIS_PUBLIC), ("b", VIS_UNKNOWN)], False,
         "public outranks unknown -- a known violation is not softened"),
        ([("a", VIS_PRIVATE), ("b", VIS_UNKNOWN)], None,
         "one unknown means the condition cannot be claimed"),
        ([("a", VIS_ABSENT), ("b", VIS_UNKNOWN)], None, "absent does not excuse unknown"),
    ]
    for results, want, label in agg_cases:
        got = worst_visibility(results)
        mark = "ok  " if got is want else "FAIL"
        if got is not want:
            bad += 1
        print(f"  [{mark}] {label} (want {want!r}, got {got!r})")

    total = len(cases) + len(agg_cases)
    print(f"self-test: {total - bad}/{total} passed")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--assume-private", action="store_true",
                    help="skip the gh query and treat the repo as private "
                         "(only when you have confirmed it another way)")
    ap.add_argument("--skip-tracker-text", action="store_true",
                    help="do not scan GitHub-side issue/comment/release text. "
                         "Only for a fast local run -- it removes the condition "
                         "that blocks publishing a dirty tracker.")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    status, ticket = read_marker(GC_REQUEST)

    results = []
    if args.assume_private:
        is_private = True
    else:
        # Addressed by immutable id; the reported name is whatever that id
        # currently answers to, so a rename shows up as drift rather than as a
        # silently-unguarded repository.
        results = []
        for repo_id, note in GUARDED_REPO_IDS:
            vis, name = query_repo_by_id(repo_id)
            label = name or f"id:{repo_id}"
            results.append((label, vis))
            print(f"[publication-block]   {label} (id {repo_id}): {vis}  — {note}")
        is_private = worst_visibility(results)

    code, msg = decide(status, ticket, is_private)

    # GitHub-side text: a second, independent condition. The orphaned-object
    # check above is about git; this is about everything that is not git.
    text_block, text_note = False, "skipped (--assume-private: offline mode)"
    if not args.assume_private and not args.skip_tracker_text:
        # Scan whatever must be clean, derived from observed visibility, plus
        # the candidate so the common private case still gets early warning.
        required, _sanctioned = slugs_requiring_clean_text(status, results)
        to_scan = sorted(set(required) | {PUBLICATION_CANDIDATE})
        clean_by_slug = {}
        for slug in to_scan:
            clean, detail = scan_tracker_text(slug)
            clean_by_slug[slug] = clean
            shown = {True: "clean", False: "DIRTY", None: "UNKNOWN"}[clean]
            print(f"[publication-block]   {slug} tracker text: {shown} — {detail}")
        text_block, text_note = decide_text(status, results, clean_by_slug)
    elif args.skip_tracker_text:
        text_note = "skipped (--skip-tracker-text)"
    print(f"[publication-block]   tracker-text verdict: {text_note}")

    if text_block and code == OK:
        code = VIOLATION
        msg = (f"{msg} BUT {text_note}. Publishing would disclose identifiers "
               "that four history rewrites removed from git but never touched "
               "in GitHub's database.")

    prefix = {OK: "[publication-block] OK:",
              VIOLATION: "[publication-block] FAIL:",
              UNKNOWN: "[publication-block] UNKNOWN:"}[code]
    print(f"{prefix} {msg}")
    if code == OK and not args.assume_private:
        # Count what was ACTUALLY checked. Reporting len(GUARDED_SLUGS) here
        # said "4" while three repositories were queried -- the same
        # report-more-than-you-checked defect that started this whole thread.
        print(f"[publication-block] checked {len(GUARDED_REPO_IDS)} repository(ies) "
              "by immutable id, plus the GitHub-side text of "
              f"{PUBLICATION_CANDIDATE}")
    return code


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
