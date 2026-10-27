# AGPL corresponding-source determination — 2026-09-06

<!-- tracking-banner: exempt -- a dated determination record; GitHub #149 is now CLOSED on this determination, and #200 carries the remaining publication prerequisites -->

**GitHub #149.** Produced by pipeline `backlog-workable-p82` phase-4.

> **UPDATE — 2026-09-16, pipeline `p86-followups` phase-2.** #149 is now **CLOSED**.
> The owner confirmed the disposition: the corresponding-source obligation is
> resolved by making the application available via this repository being made
> public. That answers the licensing question #149 asked, which is what #149 was
> for.
>
> The section below headed "Status: DECIDED, NOT DONE. #149 stays OPEN" was
> correct on 2026-09-06 and is superseded on this point only. Everything else in
> this document — the facts, and especially the blocking prerequisites — still
> stands, because closing the licensing question did not perform the publication.
>
> **The prerequisites moved rather than closed.** #149 was the only thing
> tracking them, so they are carried to **#200**, which holds all five and is the
> issue to read before anyone flips repository visibility. Read this document for
> the reasoning; read #200 for what is still owed.

> **This document records a DECISION OF DIRECTION and the facts behind it. It is
> not legal advice, and it is not written by anyone with legal standing.** The
> issue said so itself, and that has not changed.

---

## Decision

**Make the source repository public.** Chosen by the project owner on
2026-09-06, from the three options put to them (record-and-defer, add a written
offer, or publish).

The reasoning is the ordinary one: if the corresponding source is public and
reachable at the point of distribution, that ordinarily satisfies the
obligation, and it does so without maintaining an offer-fulfilment process.

**Status at the time of writing (2026-09-06): DECIDED, NOT DONE. #149 stays
OPEN.** Publication has *not* been performed, and must not be until the
prerequisites below are met.

**Status as of 2026-09-16: the licensing question is CLOSED (#149); publication
is still NOT DONE, and the prerequisites below are tracked in #200.** The second
half of that sentence is the load-bearing one — a closed #149 must not be read
as permission to publish.

---

## The facts, measured 2026-09-06

| fact | state |
|---|---|
| PDFE's own licence | AGPL-3.0-only (`vcpkg.json`, `LICENSE`) |
| MuPDF | **AGPL-3.0** — the reason the obligation attaches |
| QPDF | Apache-2.0 (no copyleft obligation) |
| `ipswitch8/PDFE` | **private** |
| v0.2.0 | **signed and distributed** |
| written offer / "corresponding source" text | **none found** in `docs/` or `packaging/`, nor in the installer or release assets |
| `THIRD-PARTY-NOTICES.md` + CI checker + checker self-test | **present and good** — better attribution hygiene than most projects manage |

The attribution side is in good order. The open question was never about
attribution; it is about *source availability to a recipient of the binary*.

---

## Prerequisites before publishing — these are blocking

Publishing is **irreversible in practice**: once public, content is cloned,
cached and indexed, and making the repository private again does not retract
what was taken. This project's own history makes that concrete.

### 1. The account-name leak history is not fully cleared

The repository had its history rewritten across **1,290 commits, over three
separate leak surfaces**, and was finally recreated to drop PR refs — all to
remove the build account name. As recently as **this pipeline** (2026-09-06,
phase-1) that string was committed again, 90 times, in a generated test-evidence
XML; it was sanitized and the commit amended and force-pushed the same day.

Crucially, per this project's own record: **GitHub continues to serve orphaned
objects by SHA after a force-push**, and clearing them requires a GitHub Support
request that, as of the 2026-08-27 rewrite, **is still outstanding**.

**Publishing before that is resolved publishes the history the rewrites were
performed to remove.**

### 2. A publication-readiness pass has not been run

At minimum, before flipping visibility:

- Confirm GitHub Support has garbage-collected the orphaned objects from the
  2026-08-27 rewrite and the 2026-09-06 amend, and verify by fetching the old
  SHAs and getting a 404.
- Run `UiSuitePathHygiene.NoTrackedFileCarriesTheBuildAccountName` over the full
  tracked tree (it passes at HEAD today) **and** sweep git *history*, not just
  the working tree — the guard checks tracked files, not past commits.
- Review what else the history contains that was never meant to be public:
  machine names, share paths (`\\SDC\...` appears in environment output),
  internal hostnames, runner registration material.
- Confirm no credential ever transited a tracked file. R2 credentials are
  documented as machine-environment-only, and registration tokens as
  never-persisted; confirm rather than assume.

### 3. Decide what "at the point of distribution" means here

Publishing the repo satisfies the obligation for recipients who can find it.
Consider whether the installer and release assets should *also* name the source
URL, so a recipient of the binary alone is not left to guess. That is cheap and
removes the ambiguity that prompted #149.

> **DONE — p100 phase-3, 2026-10-24. The mechanism chosen, in the tree and
> machine-enforced:**
>
> 1. **Inside the install tree:** `CORRESPONDING-SOURCE.txt` (repo root) is
>    `install(FILES …)`-ed next to `LICENSE` in `CMakeLists.txt`, so every
>    future installer carries the source URL even for a recipient with only a
>    copied install directory.
> 2. **On every release page:** [`docs/release-notes-template.md`](../release-notes-template.md)
>    carries a mandatory "Corresponding source" section, and
>    `tools/gate/check_release_asset_hygiene.py --release <tag>` FAILS any
>    release whose body lacks the phrase plus a `github.com/ipswitch8/PDFE`
>    URL — wired to run on `release: published/edited` in
>    `release-asset-hygiene.yml`. The same guard scans every asset's bytes
>    (installer payload extracted) for the scrub-needle inventory.
> 3. **Retroactively:** v0.3.0's release body was edited (body only — the
>    signed asset untouched) to add the section; the guard now exits 0 on it:
>    `corresponding-source statement: PRESENT`, `asset verdict: CLEAN`.
>
> This closes GitHub #200 item 5's mechanism from the working tree; the
> *visibility flip* in the sequence below remains the owner's.

---

## What I did not do, and why

I have **not** changed repository visibility. That is an outward-facing,
effectively irreversible action with unresolved prerequisites, and it is the
owner's to execute deliberately — not a side effect of a documentation phase.

I also record no legal conclusion. The direction is the owner's decision; whether
it discharges the AGPL obligation on the facts of distribution is a question for
someone with standing to answer.

---

## Recommended sequence

1. Chase the outstanding GitHub Support GC request (blocking).
2. Run the publication-readiness sweep above, including **history**, not just
   the working tree.
3. Add the source URL to the installer and release assets.
4. Flip visibility.

Step 5 originally read *"Close #149 citing this document and the completed
sweep."* That ordering was overtaken on 2026-09-16: the owner closed #149 on the
determination itself, ahead of the sweep, because #149's question was *which
route discharges the obligation* and that route is settled. Steps 1-4 are
unchanged and are now tracked by **#200**.

One consequence worth stating plainly, since it inverts the original sequence:
**the closed issue is no longer the reminder.** Anyone reaching for the
visibility switch should be sent to #200, not to #149's closed thread.

Also relevant, and found after this document was first written: the account-name
guard has a second blind spot in the opposite direction — it never inspects
build output either, where 418 occurrences across 6 shipped binaries were
measured on 2026-09-16. That is packaging rather than publication, tracked in
#199, but it is the same guard failing to cover the same string, and both should
be shut before the repository is public. See
`docs/audit/p86-followups/account-name-leak-enumeration-2026-09-16.md`.
