# `docs/` — what is in here, and what it is for

**Looking for what is still broken? That is [GitHub Issues](https://github.com/ipswitch8/PDFE/issues), not this directory.**
Read [`TRACKING.md`](TRACKING.md) first — it states the rule and the exceptions.

This directory is ~290 markdown files of **audit evidence and architecture
notes**. Almost none of it is a work list, and several documents are laid out in
a way that makes them look like one: severity columns, `OPEN` markers, effort
estimates. Every such file now carries a status banner at the top saying which
kind it is.

## Start here

| you want | read |
|---|---|
| what is still broken | [GitHub Issues](https://github.com/ipswitch8/PDFE/issues) |
| which list is authoritative, and why | [`TRACKING.md`](TRACKING.md) |
| how the product is put together | [`project-analysis-consolidated.md`](project-analysis-consolidated.md), [`architecture-notes.md`](architecture-notes.md) |
| what an audit found and why it decided that | `audit/` — see below |

## The two banners

Every file that could be mistaken for a tracker carries one of:

- **HISTORICAL EVIDENCE — this is not a work list.** The large majority. Read
  for the reasoning; do not read for current status. If a row here and a GitHub
  issue disagree, the issue wins.
- **LIVE OPERATIONAL STATE — keep maintaining this file.** A short list of files
  that CI, fixture generators or drift guards actually read. Not superseded by
  GitHub, but also not defect trackers. `TRACKING.md` enumerates them.

## Layout

- `audit/meta-audit-2026-07/` — the 2026-07 meta-audit: the `gap-register.md`
  (458 rows), `status-register.md`, the `WIN-`/`PDF-`/`SEC-` source documents,
  and `followup/` with ~52 `phase-NN-dispositions.md` recording what each
  follow-up phase decided. **The disposition files, not the register rows, are
  where closure lives.**
- `audit/dca/` — Defect Class Analysis records, one per escaped-defect class.
- `audit/round-2/`, `audit/round-3/`, `harness-audit/`, `coverage-expansion/` —
  earlier audit programmes, each with its own register and ID namespace.
- `audit/*-evidence-*.md` — per-change mutation evidence. These are records of
  one investigation, not lists.
- `archive/` — superseded documents. Some still say "Status: open backlog" in
  their body; they are not.

## A warning about numbers

Counts in these documents are timestamps, not facts — at least seven different
GoogleTest totals appear across this tree, from 2,840 to 5,955, each written as
current on its day. Re-derive rather than quote:

```
build/vs2022/bin/Debug/pdfe_tests.exe --gtest_list_tests   # never count TEST macros
gh issue list --state open --limit 400 --json number | jq length
```
