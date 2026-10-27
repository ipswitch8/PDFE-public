# Where the truth lives

**GitHub Issues at `ipswitch8/PDFE` is the canonical tracker for anything still
OPEN.** If you want to know what is broken, what is missing, or what someone
should work on next, that is the only list to read.

Everything else in `docs/` and `tests/docs/` that looks like a defect list is
**historical evidence, not a work list.** Those documents record what an audit
found on the day it ran and what was decided about it. They are worth reading
for the reasoning; they are not worth reading to find out what is still true.

Established 2026-09-02. Before that date this repo had no declared source of
truth, and it had roughly two dozen documents that could each reasonably be
mistaken for one.

---

## The rule

| question | where to look |
|---|---|
| What is still broken? | **GitHub Issues** |
| Was this ever looked at, and what was decided? | the audit registers below |
| Why was it decided that way? | the per-phase `*-dispositions.md` files |
| What is deliberately not tested, and why? | `tests/docs/known_issues.md`, `tests/KNOWN_WEAK_TESTS.txt` (see "Live, not historical") |

**If a register row and a GitHub issue disagree, the issue wins.** If a register
row describes something still broken and no issue exists, file one — and say so
in the issue, because that means the register row outlived its triage.

## What this does NOT mean

It does **not** mean the registers were wrong, or that they should be deleted.
They contain evidence that was gated, verified against the working tree, and in
several cases caught defects nothing else would have. Deleting them would throw
away the reasoning and keep only the conclusion.

It also does **not** mean every register row became an issue. It did not:

- `docs/audit/meta-audit-2026-07/gap-register.md` declares **458 rows** across
  seven ID namespaces.
- GitHub currently holds **126 issues**, of which **115** carry a `PDF-nn` /
  `WIN-nn` / `SEC-nn` register ID in the title.
- The `A1-` … `A5-` namespace (`status-register.md` declares 392 rows; 313
  distinct ids parse out of it and `gap-register.md` combined) had **no GitHub
  representation at all** until it was triaged on 2026-09-02 — see below.

Re-derive those numbers rather than trusting this paragraph:

```
grep -cE "^\|\s*[A-Z][A-Za-z0-9]*-[0-9]+" docs/audit/meta-audit-2026-07/gap-register.md
gh issue list --state all --limit 400 --json number | jq length
```

**The `A-` namespace has now been triaged** (2026-09-02): 313 rows reviewed, 30
needed a decision, 2 became issues (GitHub #127, #128), the rest were already
resolved or already tracked. The work is shown in
[`docs/audit/a-namespace-triage-2026-09-02.md`](audit/a-namespace-triage-2026-09-02.md),
including the part of the method that is a heuristic rather than proof.

The rows that never became issues are overwhelmingly ones a follow-up phase
already disposed of (CLOSED / RESOLVED / WONT-FIX) — that disposition lives in
`docs/audit/meta-audit-2026-07/followup/phase-NN-dispositions.md`, not in the
register row itself. **A register row with no issue is not evidence of untracked
work, and it is not evidence of closure either. It is evidence that you have to
go read the disposition file.** That is the cost of the old arrangement, and it
is why this file exists.

## Historical — read for reasoning, not for status

Each of these now carries a banner pointing here.

- `docs/audit/meta-audit-2026-07/gap-register.md` — the 458-row consolidated
  register, and the single most likely thing to be mistaken for canonical.
- `docs/audit/meta-audit-2026-07/status-register.md` — 392 declared `A1-`…`A5-`
  rows (313 distinct ids). Triaged 2026-09-02.
- `docs/audit/meta-audit-2026-07/{windows-qa-gaps,pdf-domain-qa-gaps,engine-security-gaps}.md`
  — the `WIN-`, `PDF-`, `SEC-` source documents.
- `docs/audit/meta-audit-2026-07/followup/` — ~52 `phase-NN-dispositions.md`
  plus `deferred-backlog.md` and `followup-pipeline-plan.md`.
- `docs/audit/round-2/register.md`, `docs/audit/round-3/register.md`
- `docs/harness-audit/flaws-register.md`, `docs/coverage-expansion/register.md`
- `docs/audit/rtm-coverage.md`, `docs/audit/audit-of-audits-2026-08.md`
- `docs/audit/seed-catalog-git.md`, `docs/audit/seed-catalog-docs.md`
- `docs/menu-wiring-gaps.md`, `docs/form-fixture-debt.md`,
  `docs/acrobat-pro-parity-status.md`, `docs/manual-verification.md`
- `docs/archive/` — everything under it, regardless of what its Status line says.

## Live, not historical — a different thing entirely

These are **not** defect trackers and are **not** superseded by GitHub. They are
operational state that code or CI reads, and they must keep being maintained:

| file | role | who reads it |
|---|---|---|
| `tests/docs/known_issues.md` | product behaviour deliberately known-not-to-work, with IDs like `PDFE-LIMITATION-*` | named by path in `.github/workflows/windows-sanitizer.yml` and `windows-fuzz.yml` |
| `tests/KNOWN_WEAK_TESTS.txt` | registry of knowingly-weak tests, under an add-one/close-one quota | `tests/docs/test_strategy.md` |
| `tools/gate/*.json` | gate baselines (mutants, flaky quarantine, perf, fz_try/RAII, reachability) | the matching `tools/gate/check_*.py` in CI |
| `tests/fixtures/coverage_inventory.json` | fixture inventory | `tests/fixtures/coverage_inventory.py`, `test_coverage_inventory_scrub.py` |
| `tests/fixtures/flaw_corpus/COVERAGE.md` and `README.md` | corpus tier counts | `readFile()` in `tests/test_doc_drift_guards.cpp:555-556` (`FlawCorpusCountDocs`) |
| `tests/fixtures/ui_audit/OBJECTNAME_AUDIT.md` | objectName inventory | `tests/test_accessible_names.cpp:192` |
| `tests/fixtures/README.md` | fixture index | `tools/gate/verify_f21_dispositions.py` — code reads it, **CI does not run that script** |

**This table was wrong once and is worth keeping honest.** It originally claimed
`tests/fixtures/**/COVERAGE.md` and `**/MATRIX_GAPS.md` generally were "read by
fixture generators and drift guards". An ad-hoc review checked and three files
carrying a LIVE banner had **no reader at all** — `tests/docs/test_strategy.md`,
`tests/fixtures/coverage_matrix.md` and
`tests/fixtures/textedit_repro/MATRIX_GAPS.md`. They have been downgraded to
HISTORICAL. Telling someone to keep maintaining a file nothing reads is the same
class of error as telling them a register is a work list.

A defect recorded ONLY in one of these is still an untracked defect. If one of
them describes something that should be fixed rather than tolerated, file an
issue and cross-reference it.

## The `#nn` ambiguity, which is a real trap

Two unrelated numbering schemes use bare `#nn`:

- **GitHub issue numbers** — e.g. `#124`, `#125`. Commits close them
  (`docs(security): close #50`).
- **The internal "Defect #nn" scheme** — e.g. `#39`, `#42`, `#48`, which appear
  in `docs/audit/older-threads-followup.md` and in `gap-register.md` rows
  *alongside* `PDF-*` / `SEC-*` ids.

These overlap in range and nothing distinguishes them in prose. When writing a
new reference, always write `GitHub #nn` or `ipswitch8/PDFE#nn` for the former,
and `Defect #nn` for the latter. The historical ones are ambiguous and will stay
that way; do not add more.

## Counts in prose go stale — this file included

Several documents state a test count or an issue count as of the day they were
written, and at least seven different GoogleTest totals appear across `docs/`,
ranging from 2,840 to 5,955. Treat every such number as a timestamp, not a fact,
and re-derive:

```
build/vs2022/bin/Debug/pdfe_tests.exe --gtest_list_tests   # never count TEST macros
gh issue list --state open --limit 400 --json number | jq length
```

`CLAUDE.md` carries the current suite baseline and is the one place worth
keeping accurate, because every session reads it.

## This is enforced

`tools/gate/check_tracking_banners.py` fails CI when a document under `docs/` or
`tests/` reads as a work list and carries neither banner nor a reasoned
exemption. It runs in `fast-checks.yml`, and its own self-tests
(`tools/gate/test_check_tracking_banners.py`) run in the same workflow.

The detector is scored rather than pattern-matched — status-token density plus
table-row count, with single-investigation records discounted 3x — and the
threshold is deliberately loose. **A guard that reddens on correct documents is
worse than no guard**, because it teaches people to bypass it. It found its own
test leftovers under `tests/_tmp/` the first time it ran for real; that
directory is now excluded.

If a file trips it and genuinely is not a work list, say so in the file:

```
<!-- tracking-banner: exempt -- a schema reference, not a work list -->
```

The reason is required. An exemption with a blank reason fails exactly like a
missing banner, because a bypass with no audit trail is the thing this whole
document exists to prevent.
